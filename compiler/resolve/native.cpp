#include "native.h"
#include "intrinsic.h"
#include "name.h"
#include "../parse/parser.h"

/*
 * Native's declarations.
 *
 * As in Core, everything that can be written in the language is written in the language. What the
 * compiler supplies is what the language has no way to say about itself, which here is every
 * operation whose meaning is the machine's rather than the program's.
 *
 * The fixed-width integer family used to be declared here, on the reasoning that a program which
 * cares how many bits a number has is a program close enough to the machine to have imported
 * Native anyway. That stopped being true once the JS target gained packing and scalarization:
 * `U8` and `I16` are how a record says it wants to be narrow, and on JS the payoff is a smaller
 * object rather than a smaller struct. Asking a pure web program to import the raw-pointer and
 * system-call module to reach them made the width types look unsafe, which they are not. They
 * are Core's now - see core.cpp's defineIntegerTypes.
 *
 * The pointer operations are *generic* intrinsics - declared here with a signature and no body,
 * and generated where they are called. A dereference is not one operation but one per element
 * type, so there is nothing to write down until a call says which; see intrinsic.h. That also
 * means none of them ever becomes a call in the IR: `*p` is a load, and `p + 1` is an add.
 *
 * Comparison is `instance Eq(Ptr(a))` and `instance Ord(Ptr(a))` - generated below rather than
 * written here, so that each method stays the one instruction it is instead of becoming a call.
 * A pointer therefore crosses into generic code that constrains Eq or Ord like any other type.
 *
 * Arithmetic is deliberately *not* `Num`. Three things say so independently: `class (FromInt(a))
 * Num(a)` would make `let p: %U8 = 4096` well-typed, `Num`'s operations are homogeneous while
 * `p + 1` and `difference(p, q)` are not, and `*` and `/` on an address mean nothing. The class
 * that fits pointer arithmetic is a heterogeneous one - roughly `class Offset(a, b)`, which would
 * also cover an iterator plus a count - and until a second type wants it the plain functions below
 * are correct and cost nothing.
 */
static const char* kNativeSource = R"NATIVE(
-- The generic spelling of the pointer sigil, for ordinary generic and constraint positions.
alias Ptr(a) = %a

{-
   Raw pointers.
  
   Every operation below is unsafe by construction: a raw pointer carries no lifetime, no
   exclusivity, and no promise that what it points at is initialized or still exists. Taking one
   of a value gives its owner writable, stable-address representation requirements, since the
   memory a pointer names is always mutable.
-}

-- Reads what a pointer points at. Written `*p`.
fn *(it: %a) -> a

-- Writes through a pointer. There is no assignment form for this because `*p = v` is already one:
-- a dereference in assignment position names storage rather than producing a value.
fn store(to: %a, value: a) -> {}

-- The address of a value. A value this is applied to cannot stay in a register, so it is what
-- forces storage to exist for something that would otherwise have had none.
fn addressOf(it: a) -> %a

-- Reinterprets what a pointer points at, and the two conversions between a pointer and the
-- integer holding the same address. None of the three moves any bits.
fn cast(it: %a) -> %b
fn asInt(it: %a) -> I64
fn asPtr(it: I64) -> %a

-- The null pointer, and the test for it. `null` needs its type from context - an assignment, a
-- return, or an ascription - which is why the test exists separately: `isNull(p)` needs nothing.
fn null() -> %a
fn isNull(it: %a) -> Bool

{-
   A borrow of what a pointer names.

   The one operation that re-enters the checked world from the unchecked one. Nothing here verified
   that the pointer is valid, that what it names is initialized, or that nothing else writes through
   it while the borrow is live - which is exactly what makes it Native's and not the language's. A
   collection written over raw storage needs it, and having written it, owes its callers a signature
   whose `return` marker says what the result is rooted in.
-}
fn borrow(return it: %a) -> &a
fn borrowMut(return it: %a) -> &a

-- The size and alignment of a value's type, in bytes.
fn sizeOf(it: a) -> I64
fn alignOf(it: a) -> I64

-- Pointer arithmetic, in elements rather than in bytes: `p + 1` advances by one `a`, whatever an
-- `a` is. Byte-granular work casts to `%U8` first, where the two coincide.
fn +(it: %a, count: I64) -> %a
fn -(it: %a, count: I64) -> %a
fn difference(from: %a, to: %a) -> I64

{-
   Memory and the operating system.
-}

-- The two block operations. Neither checks that the regions are distinct or that either is large
-- enough; `copyMemory` is the non-overlapping form.
fn copyMemory(to: %U8, from: %U8, count: I64) -> {}
fn setMemory(to: %U8, value: U8, count: I64) -> {}

-- The system call intrinsic, at each arity a call needs. Design.md's "Interfacing the OS" builds
-- the OS interface as a thin template over exactly this; the arguments and the result are plain
-- integers because that is what the kernel ABI passes, and a pointer reaches one through asInt.
fn syscall0(number: I64) -> I64
fn syscall1(number: I64, a: I64) -> I64
fn syscall2(number: I64, a: I64, b: I64) -> I64
fn syscall3(number: I64, a: I64, b: I64, c: I64) -> I64
fn syscall4(number: I64, a: I64, b: I64, c: I64, d: I64) -> I64
fn syscall5(number: I64, a: I64, b: I64, c: I64, d: I64, e: I64) -> I64
fn syscall6(number: I64, a: I64, b: I64, c: I64, d: I64, e: I64, f: I64) -> I64

{-
   The heap.
  
   One mapped region, carved from the front: a table of free-list heads, then a bump area. An
   allocation is a header word holding its size class followed by the payload the caller is given,
   so every payload is 8-byte aligned and every block is a power of two at least 16 bytes.
  
   Freeing threads the block onto the free list for its class, using the first 8 bytes of the
   payload as the next pointer - which is why a block is never smaller than 16 bytes, and why the
   header is not written again when the block is reused: it still says what it said.
  
   A single region and no unmapping is the whole of the policy. Design.md's runtime memory
   reclamation is about *when* a value is released, which the compiler decides; this is only where
   the bytes come from.
-}

-- 4 MiB of address space, and 32 size classes from 16 bytes up. The table is 256 bytes, so the
-- bump area starts there. An immutable `let` is a name for a constant and occupies nothing, so
-- these two are the numbers themselves wherever they are read; the three `let &` below are the
-- static storage the policy actually needs.
let heapRegionSize = 4194304 :: I64
let heapClassCount = 32 :: I64

let &heapNext = 0 :: %U8
let &heapLimit = 0 :: %U8
let &heapFree = 0 :: Ptr(Ptr(U8))

fn initHeap() -> {}:
    let region = mapMemory(heapRegionSize)
    if isNull(region) then return

    let table = heapClassCount * 8
    setMemory(region, 0, table)

    heapFree = cast(region) :: Ptr(Ptr(U8))
    heapNext = region + table
    heapLimit = region + heapRegionSize

-- The size class of a request: the number of doublings from 16 bytes needed to hold it.
fn heapClassOf(total: I64) -> I64:
    let &size = 16 :: I64
    let &sizeClass = 0 :: I64

    while size < total:
        size = size + size
        sizeClass = sizeClass + 1

    return sizeClass

fn heapBlockSize(sizeClass: I64) -> I64 = (16 :: I64) `shl` sizeClass

fn freeListHead(sizeClass: I64) -> %U8 = *(heapFree + sizeClass)
fn setFreeListHead(sizeClass: I64, block: %U8) -> {} = store(heapFree + sizeClass, block)

-- Allocates `size` bytes, 8-byte aligned, or null when the region is exhausted or the request is
-- larger than the largest size class.
fn allocateHeap(size: I64) -> %U8:
    if isNull(heapFree):
        initHeap()
        if isNull(heapFree) then return null()

    let sizeClass = heapClassOf(size + 8)
    if sizeClass >= heapClassCount then return null()

    -- A reused block already has its header; only the free list changes.
    let reused = freeListHead(sizeClass)
    if !isNull(reused):
        setFreeListHead(sizeClass, *(cast(reused) :: Ptr(%U8)))
        return reused

    let block = heapNext
    let blockSize = heapBlockSize(sizeClass)
    if block + blockSize > heapLimit then return null()

    heapNext = block + blockSize
    store(cast(block) :: Ptr(I64), sizeClass)

    return block + 8

-- Returns an allocation to the free list of its own size class. The pointer must be one
-- allocateHeap returned and must not be freed twice; nothing here checks either.
fn freeHeap(allocation: %U8) -> {}:
    if isNull(allocation) then return

    let sizeClass = *(cast(allocation - 8) :: Ptr(I64))

    store(cast(allocation) :: Ptr(%U8), freeListHead(sizeClass))
    setFreeListHead(sizeClass, allocation)

{-
   Runs of slots - Implementation-Containers.md §2.
-}

{-
   How a stored count is represented, and why it is neither `Int` nor a whole word.

   **Unsigned**, because a count has no negative values and the sign bit is the only thing signedness
   buys it. What that is worth is not the extra bit: it is that `index >= length` on unsigned
   operands is the *whole* bounds test, since a negative index converted to this type is a very large
   one and fails the same comparison. Two tests become one everywhere a container checks an index,
   which is the same trick every bounds check in every systems language uses and the reason JS
   specifies `.length` as a `uint32` rather than as a number that happens not to go below zero.

   **Thirty-one bits and not thirty-two**, because a capacity shares its word with the one bit below
   and thirty-one is what is left. That bit is the whole of the difference between this cap and a
   `U32`'s: 2147483647 elements is two gibibytes of `[U8]` and eight of `[Int]`, since the bound is on
   the element count rather than on the byte size. A `resize` past it fails in the same way one the
   allocator refused does, and lifting it properly means a wider count, which is a Repr variant
   (Implementation-Containers.md §9) rather than a bit that can be found somewhere.

   A plain `alias` and not `alias qualified`: this is a width, not a new type, and every `Int` that
   reaches it should convert by writing `::` rather than by unwrapping a newtype.
-}
{-
   How wide a *borrowed* container's length is, and why it is not `Count`.

   `Count` below is what an owner *stores*, and §9's whole design is that an owner's representation
   varies - per implementation, per target, per root. A **borrow** is the opposite: `Flat(a)` is the
   single representation every `[T]` signature in every program shares, so whatever it can describe
   is the ceiling for every container that will ever be passed as `[T]`. A thirty-one-bit borrow
   would make the *universal* thing the limiting one, which inverts §1 - an owner could be as large
   as it liked and simply never be borrowable, which is what a `LargeArray` would have run into.

   So a slice's length is `Size`, Core's word-width index type - see the note where it is bound. The
   cost natively is a sign-extension where an `Int` index meets a `Size` bound, and four bytes on a
   *stored* slice; both are named in Implementation-Containers.md §4.4.
-}

alias Count = @bits(31) U32

{-
   Whether this run's storage is the allocator's, as the one bit that is actually asked for.

   `InstAlloc::storageFlag` chooses between four storage classes and this used to record all four,
   which cost two bits and left the capacity thirty. But nothing ever asks *which* class - `resize`
   and `releaseRun` both ask "is this mine to hand back", and inline, frame and region storage answer
   no for three different reasons that are none of a run's business. One bit answers the question
   that is asked, and the bit it gives back doubles what a container can hold.

   What is lost is a printed IR that named the class. The escape analysis still decides between four;
   it writes the answer to this question rather than the decision itself.
-}
alias HeapFlag = @bits(1) U32

-- The largest count either field can hold, as the number a caller is checked against rather than as
-- a mask applied behind its back. `@bits` stores truncate silently, which is tolerable for an
-- integer and corrupts a container for a length - Implementation-Containers.md §7.1.
let maxCount = 2147483647 :: Int

{-
   How many bytes `count` elements occupy.

   Written as a pointer difference rather than with `sizeOf`, because what is wanted is the size of
   a *type* and pointer arithmetic already scales by exactly that - `from` is never read, only
   measured against. Counts are `Int` and byte quantities are `I64`, which is the split the two
   sides of this function have: an index is a number of elements the program wrote, and a size is
   what the allocator and the block operations take.
-}
fn byteSpan(from: %a, count: Int) -> I64 =
    difference(cast(from) :: %U8, cast(from + count) :: %U8)

{-
   An owned, uninitialized run of slots, plus where it came from.

   Two words and no policy. It does not know which of its slots hold values, it cannot be pushed
   to, and it will not grow on its own - which is the whole point: occupancy is a private matter for
   whichever container is built on top of it, so a count, a bitmap, a sentinel and a free list are
   all equally expressible and none of them is in the primitive.

   `ownsHeap` is what the compiler decided about where these slots live, reduced to the one question
   anyone asks of it - see InstAlloc::storageFlag and `HeapFlag`. It is the only thing here the
   program did not write, and it exists so that `Reclaim` below is a test rather than a guess: the
   same two words describe a run inlined into its owner, one on the frame, one in a region and one
   from the allocator, and only the last of those is anyone's to hand back.

   Two words and not three, because the capacity and that bit share one, and the bit is deliberately
   at the *end* of the word rather than in the pointer. The low bits of `items` would have held it
   and would have bought a thirty-second bit of count, at the price of a `%a` that is not an address:
   `items + index` is the most frequent operation on this field, a low tag does not survive it at a
   stride of one, and every run would have to be over-aligned to make room. See
   Implementation-Containers.md §10.3.
-}
data Run(a) {items: %a, capacity: Count, ownsHeap: HeapFlag}

-- The two answers, and there are two rather than four for the reason `HeapFlag` gives. `runBorrowed`
-- covers inline, frame and region storage together: each is handed back by something that is not
-- this run - the owner's own bytes, the frame returning, the region closing - and telling them apart
-- would be a distinction nothing acts on.
let runBorrowed = 0 :: HeapFlag
let runFromHeap = 1 :: HeapFlag

-- A run with room for nothing, which allocates nothing. Every container's empty value starts here.
fn emptyRun() -> Run(a) = Run {items: null(), capacity: 0, ownsHeap: runBorrowed}

{-
   A run of `capacity` slots, placed by the compiler.

   An intrinsic, because this is the one operation the language has no way to say about itself: what
   it expands to is an ordinary allocation with a count beside it (InstAlloc::extent), so where the
   slots live is decided by the same escape analysis that places every other allocation and the bit
   it writes into `ownsHeap` is that decision reduced to what a run acts on.
-}
fn newRun(capacity: Int) -> Run(a)

fn capacity(self: Run(a)) -> Int = self.capacity :: Int

-- Where the slots start. A container indexes off this; nothing here says how many of them hold
-- anything, because a run does not know.
fn slots(self: Run(a)) -> %a = self.items

{-
   Room for `wanted` slots, relocating if there is not.

   `&self` is what makes relocation safe with no new rule: a mutable borrow of the run conflicts with
   any live borrow into it, so the checker already rejects holding an element borrow across one of
   these. False means the allocator refused and the run is exactly as it was.

   The whole of the old capacity is copied rather than the live prefix, because a run does not know
   which of its slots are live - that is what §2 means by having no notion of occupancy. It costs a
   constant factor on a growth that is amortized anyway, and it is what keeps the primitive from
   needing a count it would then have to be told about.

   A `wanted` past what `Count` can hold is refused rather than truncated, which is the same answer
   an allocator that said no gets and for a better reason: storing it would leave a run whose
   capacity field disagreed with the storage behind it, and every later `resize` would read the
   masked number back and believe it.
-}
fn resize(&self: Run(a), wanted: Int) -> Bool:
    let room = self.capacity :: Int
    if wanted <= room then return True
    if wanted > maxCount then return False

    let fresh = cast(allocateHeap(byteSpan(self.items, wanted))) :: %a
    if isNull(fresh) then return False

    if room > 0:
        copyMemory(cast(fresh) :: %U8, cast(self.items) :: %U8, byteSpan(self.items, room))

    if self.ownsHeap == runFromHeap then freeHeap(cast(self.items) :: %U8)

    self.items = fresh
    self.capacity = wanted :: Count
    self.ownsHeap = runFromHeap

    return True

{-
   The placement test - Implementation-Storage.md §5.

   Written as a plain function taking the run by borrow as well as as the instance below, because a
   container built on runs has to be able to release its own without owning it out of itself: moving
   a field out of the value an authored `Reclaim` was handed is a partial move, and duplicating this
   comparison in every such container is the thing worth avoiding. It is storage release and nothing
   else, which is what makes it a permitted call inside an authored `Reclaim` - see
   checkReclaimShape.

   Inline, frame and region storage release nothing: the owner's own bytes, the frame returning, and
   the region closing are what hand those back. Only the allocator's is this function's to give
   back, which is why the three are one value here. When the bit is a constant the escape analysis
   patched - which it is everywhere the run was not built inside a generic body - the comparison
   folds and this whole function with it.

   A `Reclaim` and not a `Drop`, which is what keeps a container built on runs region-placeable: a
   region discharges every `Reclaim` inside it in bulk, and handing storage back is exactly the kind
   of thing that may happen in bulk at a point the author did not choose.

   **This is one comparison and it is no longer a call**, which is Implementation-Containers.md
   §13.2's first step: a callee taking an aggregate by the default convention is a memory-typed value
   parameter, and the inliner used to decline every one of those. It re-roots them at the caller's
   own place now (`Binding::Memory` in opt_inline.cpp), so this test is spliced into whatever reads
   it, and the teardown holding it is spliced into the `drop` that runs it.

   And it *folds*, which §2 has claimed all along and which was not true until §13.2 landed: the tag
   is a constant in the literal's own `Run` allocation and reaches the array through whole-aggregate
   copies, so opt_place.cpp carries what is known across them rather than rewriting either side. A
   frame-placed array's teardown is no instructions at all. What still tests at run time is an array
   something wrote through a borrow, and a grown one - where the question is real.
-}
fn releaseRun(self: Run(a)) -> {}:
    if self.ownsHeap == runFromHeap then freeHeap(cast(self.items) :: %U8)

instance Reclaim(Run(a)):
    fn reclaim(->value: Run(a)) -> {} = releaseRun(value)

{-
   The representations - Implementation-Containers.md §3.

   A container author needs access to a run's contents, and a single raw pointer cannot serve: a
   narrow-element run has no element address, and a run whose count was packed into its owner has no
   self-contained descriptor to point at. So the shapes are named types, each with a coherent
   operation set, and an author dispatches on the representation rather than testing for it - a
   UTF-8 decoder takes a `Flat(U8)`, a bitset scan will take a `Bits(Bool)`.

   `Flat(a)` is also what a borrow of `[a]` *is*. That is not two facts: §4's slice Repr and this
   surface are the same shape named twice, which is the whole reason the borrow of a container never
   dispatches. `Bits(a)` is the narrow-element form and waits on §11's fractional stride.

   It owns nothing. Every field is a copy of something whose lifetime belongs to whoever the slice
   was taken from, which is what makes it TrivialCopy and what the borrow at the point it was made
   is responsible for.
-}
data Flat(a) {items: %a, length: Size}

-- The element address a `Flat` is defined by. Absent on `Bits` when that lands, deliberately: a
-- narrow element has no address, and the partiality is what keeps `sizeOf` and pointer arithmetic
-- off the fractional-stride path.
fn values(self: Flat(a)) -> %a = self.items
)NATIVE";

/*
 * Native.Linux.
 *
 * The platform half: the system call numbers of one kernel and ABI, and nothing else. Everything
 * it is written in terms of comes from Native, and the one thing Native needs from it is
 * mapMemory - which is why the two import each other rather than layering one over the other.
 */
static const char* kLinuxSource = R"LINUX(
import Native

-- amd64 Linux call numbers.
let sysMmap = 9 :: I64
let sysMunmap = 11 :: I64
let sysWrite = 1 :: I64
let sysExit = 60 :: I64

-- PROT_READ | PROT_WRITE, and MAP_PRIVATE | MAP_ANONYMOUS.
let protReadWrite = 3 :: I64
let mapPrivateAnonymous = 34 :: I64

-- Maps `size` bytes of zeroed, readable and writable address space, or null if the kernel
-- refused. mmap reports failure as a small negative value rather than as an error flag, which is
-- why the result is checked as a number before it becomes a pointer.
fn mapMemory(size: I64) -> %U8:
    let result = syscall6(sysMmap, 0, size, protReadWrite, mapPrivateAnonymous, -1, 0)
    if result < 0 then return null()

    return asPtr(result)

fn unmapMemory(from: %U8, size: I64) -> I64 = syscall2(sysMunmap, asInt(from), size)

fn writeFile(handle: I64, from: %U8, count: I64) -> I64 = syscall3(sysWrite, handle, asInt(from), count)

fn exitProcess(status: I64) -> {}:
    syscall1(sysExit, status)
    return
)LINUX";

/*
 * The pointer intrinsics.
 */

namespace {

// A pointer operation's element type: what the first argument points at. Every intrinsic below
// needs it, and taking it from the argument rather than from the substituted type arguments is
// what lets one emitter serve a signature whose result is a pointer and one whose result is not.
static TypePtr elementType(ExprResolver& resolver, Buffer<ModulePtr<Value>> args) {
    return pointeeType(resolver.global, resolver.valueType(args[0]));
}

static ModulePtr<Value> emitDeref(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                  LocationId source, StringId name) {
    return resolver.load(Place::atPointer(args[0]), source, name);
}

// An assignment rather than an initialization: what a raw pointer names is memory the program
// manages itself, so whatever was there is being overwritten. Nothing is dropped either way - a
// pointer root is outside the ownership graph entirely, which is what makes this the unsafe module.
static ModulePtr<Value> emitStore(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                  LocationId source, StringId) {
    resolver.assign(Place::atPointer(args[0]), args[1], source);
    return nullptr;
}

static ModulePtr<Value> emitAddressOf(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                      LocationId source, StringId name) {
    return resolver.addressOf(resolver.materialize(args[0], source), source, name);
}

// One machine word reinterpreted as another. The three signatures that reach here - pointer to
// pointer, pointer to integer, integer to pointer - are the same instruction, which lowering
// turns into a bitcast because nothing about the bits changes.
static ModulePtr<Value> emitReinterpret(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                        LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstUnary>(source, name, type, Value::Cast, args[0]));
}

// The lower IR has no pointer immediates on purpose, so a null pointer is the integer zero
// reinterpreted - which is what `asPtr(0)` says anyway.
static ModulePtr<Value> emitNull(ExprResolver& resolver, Buffer<ModulePtr<Value>>, TypePtr type,
                                 LocationId source, StringId name) {
    auto zero = resolver.makeInt(source, resolver.module.scalar.long_, 0);
    return resolver.ref(resolver.emit<InstUnary>(source, name, type, Value::Cast, zero));
}

// And the test for one goes the other way: the address as a number, against zero.
static ModulePtr<Value> emitIsNull(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    auto address = resolver.module.scalar.long_;
    auto number = resolver.ref(resolver.emit<InstUnary>(source, 0, address, Value::Cast, args[0]));
    auto zero = resolver.makeInt(source, address, 0);

    return resolver.ref(resolver.emit<InstCmp>(source, name, type, number, zero, CompareOp::Eq));
}

// `borrow(p)` and `borrowMut(p)`. The place is the memory the pointer names, so the borrow is
// rooted where the pointer was rooted and everything downstream of it - the return-root check, the
// caller's loan - follows from that one fact.
template<bool mut>
static ModulePtr<Value> emitBorrowAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstBorrow>(source, name, type, Place::atPointer(args[0]), mut));
}

/*
 * `sizeOf(x)` and `alignOf(x)`.
 *
 * A question rather than an answer, now that layout is a target's business: the instruction carries
 * the type and whoever emits folds it. That costs nothing on the concrete path - it is one immediate
 * either way - and it is what makes these work inside a generic body at all, where there is no
 * number to fold and the width comes out of the caller's TypeDesc instead.
 */
static ModulePtr<Value> emitSizeOf(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstTypeMetric>(source, name, type, resolver.valueType(args[0]),
                                                      TypeMetricKind::Size));
}

static ModulePtr<Value> emitAlignOf(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                    LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstTypeMetric>(source, name, type, resolver.valueType(args[0]),
                                                      TypeMetricKind::Align));
}

/*
 * `p + n` and `p - n`, in elements.
 *
 * The scale used to be folded here, and skipped entirely for a one-byte element - which is what made
 * `%U8` the type byte arithmetic is written in without paying for a multiply. Both halves of that
 * moved rather than disappeared: the scale is a TypeMetric, and the multiply by the one it folds to
 * is removed where every other constant-folding decision is made, in the translation to the lower IR.
 * The resolver no longer claims to know how wide a `U8` is on the target being built for.
 */
template<Value::Kind kind>
static ModulePtr<Value> emitPointerOffset(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                          LocationId source, StringId name) {
    auto offsetType = resolver.valueType(args[1]);
    auto scale = resolver.ref(resolver.emit<InstTypeMetric>(source, 0, offsetType,
                                                            elementType(resolver, args),
                                                            TypeMetricKind::Stride));
    auto offset = resolver.ref(resolver.emit<InstBinary>(source, 0, offsetType, Value::Mul,
                                                         args[1], scale));

    return resolver.ref(resolver.emit<InstBinary>(source, name, type, kind, args[0], offset));
}

static ModulePtr<Value> emitDifference(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto bytes = resolver.ref(resolver.emit<InstBinary>(source, 0, type, Value::Sub, args[1], args[0]));
    auto scale = resolver.ref(resolver.emit<InstTypeMetric>(source, 0, type,
                                                            elementType(resolver, args),
                                                            TypeMetricKind::Stride));

    return resolver.ref(resolver.emit<InstBinary>(source, name, type, Value::Div, bytes, scale));
}

/*
 * `newRun(n)` - Implementation-Containers.md §2.
 *
 * An intrinsic rather than a function, because what it expands to is an allocation with a count and
 * the language has no spelling for one. It is the *only* thing here that is compiler magic: every
 * other operation on a run - the empty one, the capacity, the address of the slots, growth, and the
 * placement switch its `Reclaim` is - is written in the language above, over this and over the
 * allocator.
 *
 * The result type is what says which element it is a run of. Nothing in the argument list does, and
 * nothing needs to: `newRun()` at a `Run(Buffer)` is the same call at a `Run(U8)` with a different
 * stride, which is exactly the shape a generic intrinsic has.
 */
static ModulePtr<Value> emitNewRun(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId) {
    ModulePtr<Value> items = nullptr;
    auto run = resolver.buildRun(type, args[0], source, items);

    if(!run) {
        resolver.context.diagnostics.error("internal: newRun's result is not a run of slots"_v, source);
    }

    return run;
}

template<NativeOp op>
static ModulePtr<Value> emitNativeOp(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto instruction = resolver.create<InstNative>(source, name, type, op);
    for(auto arg: args) instruction->args.push(resolver.module.arena, arg);

    resolver.append(instruction);
    return isUnit(resolver.global, type) ? nullptr : resolver.ref(instruction);
}

} // namespace

/*
 * Assembling the modules.
 */

/*
 * Eq and Ord over every pointer type at once.
 *
 * The head is `Ptr(a)`, written here as a generic context holding one variable and the pointer
 * type over it. Nothing about comparing two addresses depends on what they point at, so the
 * instance has no requirement of its own to prove and is the simplest possible parametric head -
 * which is what makes it the natural first client of one.
 *
 * Every method is an intrinsic, so selecting the instance expands to the same `cmp` a concrete
 * instance would; `compare` is the one method with a real body, and it is specialized per pointee
 * type the way any generic function is.
 */
static void definePointerInstances(Module& module) {
    auto global = *module.types;
    auto env = new (module.types) GenEnv(GenEnv::Instance);
    auto envPointer = env - global;

    auto name = module.context.addQualifiedName("a", 1, 1);
    auto variable = new (module.types) GenType(envPointer, name, 0);
    env->types.push(module.types, variable - global);

    auto pointer = resolvePointerType(module, (Type*)variable - global);

    defineEq(module, pointer, envPointer);
    defineOrd(module, pointer, envPointer);
}

static void attachPointerIntrinsics(Module& module) {
    attachIntrinsic(module, "*"_v, emitDeref);
    attachIntrinsic(module, "store"_v, emitStore);
    attachIntrinsic(module, "addressOf"_v, emitAddressOf);
    attachIntrinsic(module, "cast"_v, emitReinterpret);
    attachIntrinsic(module, "asInt"_v, emitReinterpret);
    attachIntrinsic(module, "asPtr"_v, emitReinterpret);
    attachIntrinsic(module, "null"_v, emitNull);
    attachIntrinsic(module, "isNull"_v, emitIsNull);
    attachIntrinsic(module, "borrow"_v, emitBorrowAt<false>);
    attachIntrinsic(module, "borrowMut"_v, emitBorrowAt<true>);

    // `borrowMut` is declared `-> &a` like its immutable sibling, because the grammar has one
    // spelling for a borrow type; which of the two it is comes from the signature it appears in,
    // and this one has no `return` group to say so. So it is said here instead.
    if(auto found = module.functions.get(module.context.addUnqualifiedName("borrowMut", 9))) {
        auto function = (*module.arena)[found.unwrap()];
        auto declared = (BorrowType*)(*module.types)[function->returnType];
        function->returnType = resolveBorrowType(module, declared->to, true);
    }

    attachIntrinsic(module, "sizeOf"_v, emitSizeOf);
    attachIntrinsic(module, "alignOf"_v, emitAlignOf);

    attachIntrinsic(module, "+"_v, emitPointerOffset<Value::Add>);
    attachIntrinsic(module, "-"_v, emitPointerOffset<Value::Sub>);
    attachIntrinsic(module, "difference"_v, emitDifference);

    attachIntrinsic(module, "newRun"_v, emitNewRun);

    attachIntrinsic(module, "copyMemory"_v, emitNativeOp<NativeOp::CopyMemory>);
    attachIntrinsic(module, "setMemory"_v, emitNativeOp<NativeOp::SetMemory>);

    static const StringView syscalls[] = {
        "syscall0"_v, "syscall1"_v, "syscall2"_v, "syscall3"_v,
        "syscall4"_v, "syscall5"_v, "syscall6"_v,
    };

    for(auto& name: syscalls) attachIntrinsic(module, name, emitNativeOp<NativeOp::Syscall>);
}

static ast::Module* parseEmbedded(Context& context, const char* text, StringView name) {
    auto id = context.addQualifiedName(name.ptr, name.length);
    Lexer lexer(context, context.diagnostics, StringView { text, stringLength(text) }, id);
    Parser parser(context, lexer, id);
    parser.allowSignatures = true;

    return new ast::Module(parser.parseModule());
}

void defineNative(Program& program) {
    auto& context = program.context;

    auto nativeAst = parseEmbedded(context, kNativeSource, "Native"_v);
    auto native = program.addModule(nativeAst->name, *nativeAst->region);
    program.embeddedAsts.push(nativeAst);
    program.native = native;

    // The types have to exist before the signatures that name them are read, and the instances
    // before any body that uses one - which is the same order Core is built in. Core has to be
    // imported first of all, since the classes these instances join are its.
    resolveImports(*native, *nativeAst, nullptr);
    definePointerInstances(*native);

    resolveModuleDecls(*native, *nativeAst, nullptr, true);
    attachPointerIntrinsics(*native);

    // Native.Linux is resolved second, so its `import Native` finds a module that already exists
    // rather than asking the provider for one. Native's own use of mapMemory is then made visible
    // by hand: the two halves refer to each other, and an import statement in Native could only
    // name a platform that has not been chosen yet.
    auto linuxAst = parseEmbedded(context, kLinuxSource, "Native.Linux"_v);
    auto platformModule = program.addModule(linuxAst->name, *linuxAst->region);
    program.embeddedAsts.push(linuxAst);

    auto& platform = *native->imports.push();
    platform.module = platformModule;
    platform.localName = platformModule->name;

    resolveModuleDecls(*platformModule, *linuxAst, nullptr);

    // Recorded so that storage-class selection and drop insertion can emit calls to them without
    // going through name resolution in whichever module happened to need one - see Program.
    auto findNative = [&](const char* text, Size length) -> ModulePtr<Function> {
        auto found = native->functions.get(context.addUnqualifiedName(text, length));
        return found ? found.unwrap() : nullptr;
    };

    program.allocateHeap = findNative("allocateHeap", 12);
    program.freeHeap = findNative("freeHeap", 8);
    program.releaseRun = findNative("releaseRun", 10);

    // And the run and the slice, for the same reason - see Program::runType and Program::sliceType.
    auto named = [&](const char* text, Size length) -> GlobalPtr<RecordType> {
        auto found = native->namedTypes.get(context.addQualifiedName(text, length, 1));
        if(!found) return nullptr;

        return (RecordType*)(*program.types)[found.unwrap()] - *program.types;
    };

    program.runType = named("Run", 3);
    program.sliceType = named("Flat", 4);
}
