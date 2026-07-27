#include "native.h"
#include "intrinsic.h"
#include "name.h"
#include "../parse/parser.h"

/*
 * Native's declarations.
 *
 * As in Core, everything that can be written in the language is written in the language. What the
 * compiler supplies is what the language has no way to say about itself, which here is a longer
 * list than Core's: the fixed-width integer types, and every operation whose meaning is the
 * machine's rather than the program's.
 *
 * The pointer operations are *generic* intrinsics - declared here with a signature and no body,
 * and generated where they are called. A dereference is not one operation but one per element
 * type, so there is nothing to write down until a call says which; see intrinsic.h. That also
 * means none of them ever becomes a call in the IR: `*p` is a load, and `p + 1` is an add.
 *
 * The comparison and arithmetic operators on pointers are plain functions rather than Eq/Ord/Num
 * instances, because an instance head must be a concrete type and `%a` is not one. They join the
 * overload set for their names on equal terms with the classes (see emitCall's R5), so `a == b`
 * still means Eq for everything that has an instance and pointer comparison for what does not.
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

-- The size and alignment of a value's type, in bytes.
fn sizeOf(it: a) -> I64
fn alignOf(it: a) -> I64

-- Pointer arithmetic, in elements rather than in bytes: `p + 1` advances by one `a`, whatever an
-- `a` is. Byte-granular work casts to `%U8` first, where the two coincide.
fn +(it: %a, count: I64) -> %a
fn -(it: %a, count: I64) -> %a
fn difference(from: %a, to: %a) -> I64

fn ==(lhs: %a, rhs: %a) -> Bool
fn !=(lhs: %a, rhs: %a) -> Bool
fn <(lhs: %a, rhs: %a) -> Bool
fn <=(lhs: %a, rhs: %a) -> Bool
fn >(lhs: %a, rhs: %a) -> Bool
fn >=(lhs: %a, rhs: %a) -> Bool

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
-- bump area starts there.
let &heapNext = 0 :: %U8
let &heapLimit = 0 :: %U8
let &heapFree = 0 :: Ptr(Ptr(U8))

fn heapRegionSize() -> I64 = 4194304
fn heapClassCount() -> I64 = 32

fn initHeap() -> {}:
    let region = mapMemory(heapRegionSize())
    if isNull(region) then return

    let table = heapClassCount() * 8
    setMemory(region, 0, table)

    heapFree = cast(region) :: Ptr(Ptr(U8))
    heapNext = region + table
    heapLimit = region + heapRegionSize()

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
    if sizeClass >= heapClassCount() then return null()

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
fn sysMmap() -> I64 = 9
fn sysMunmap() -> I64 = 11
fn sysWrite() -> I64 = 1
fn sysExit() -> I64 = 60

-- PROT_READ | PROT_WRITE, and MAP_PRIVATE | MAP_ANONYMOUS.
fn protReadWrite() -> I64 = 3
fn mapPrivateAnonymous() -> I64 = 34

-- Maps `size` bytes of zeroed, readable and writable address space, or null if the kernel
-- refused. mmap reports failure as a small negative value rather than as an error flag, which is
-- why the result is checked as a number before it becomes a pointer.
fn mapMemory(size: I64) -> %U8:
    let result = syscall6(sysMmap(), 0, size, protReadWrite(), mapPrivateAnonymous(), -1, 0)
    if result < 0 then return null()

    return asPtr(result)

fn unmapMemory(from: %U8, size: I64) -> I64 = syscall2(sysMunmap(), asInt(from), size)

fn writeFile(handle: I64, from: %U8, count: I64) -> I64 = syscall3(sysWrite(), handle, asInt(from), count)

fn exitProcess(status: I64) -> {}:
    syscall1(sysExit(), status)
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

static ModulePtr<Value> emitStore(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                  LocationId source, StringId) {
    resolver.initialize(Place::atPointer(args[0]), args[1], source);
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

static ModulePtr<Value> emitSizeOf(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId) {
    return resolver.makeInt(source, type, typeSize(resolver.global, resolver.valueType(args[0])));
}

static ModulePtr<Value> emitAlignOf(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                    LocationId source, StringId) {
    return resolver.makeInt(source, type, typeAlign(resolver.global, resolver.valueType(args[0])));
}

// `p + n` and `p - n`, in elements. The scale is folded away when the element is a byte, which is
// what makes `%U8` the type byte arithmetic is written in without paying for a multiply.
template<Value::Kind kind>
static ModulePtr<Value> emitPointerOffset(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                          LocationId source, StringId name) {
    auto size = typeSize(resolver.global, elementType(resolver, args));
    auto offset = args[1];

    if(size != 1) {
        auto scale = resolver.makeInt(source, resolver.valueType(offset), size);
        offset = resolver.ref(resolver.emit<InstBinary>(source, 0, resolver.valueType(offset),
                                                        Value::Mul, offset, scale));
    }

    return resolver.ref(resolver.emit<InstBinary>(source, name, type, kind, args[0], offset));
}

static ModulePtr<Value> emitDifference(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto size = typeSize(resolver.global, elementType(resolver, args));
    auto bytes = resolver.ref(resolver.emit<InstBinary>(source, size == 1 ? name : 0, type,
                                                        Value::Sub, args[1], args[0]));

    if(size == 1) return bytes;

    auto scale = resolver.makeInt(source, type, size);
    return resolver.ref(resolver.emit<InstBinary>(source, name, type, Value::Div, bytes, scale));
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

// One fixed-width integer type, as Design.md's `I8`/`U8` through `I64`/`U64`. `bits` is its size
// in memory and the width is the primitive it occupies once loaded, so everything below 64 bits
// arrives in a 32-bit register and only the widest family needs a wider one.
static TypePtr addInteger(Module& module, StringView name, U16 bits, bool isSigned) {
    auto id = module.context.addQualifiedName(name.ptr, name.length, 1);
    auto width = bits == 64 ? IntType::Long : IntType::Int;
    auto type = new (module.types) IntType(bits, width, isSigned, id);

    auto pointer = (Type*)type - *module.types;
    module.namedTypes.add(id, pointer);
    return pointer;
}

// Whether a value of `from` fits in `to` without losing anything: more bits, or the same bits
// without a sign to lose. This decides which of the two conversion ladders a pair joins, which
// is the whole of the rule for whether a conversion happens on its own or has to be written.
static bool widens(GlobalBase global, TypePtr from, TypePtr to) {
    auto source = (IntType*)global[from];
    auto target = (IntType*)global[to];

    if(source->isSigned && !target->isSigned) return false;
    if(source->isSigned == target->isSigned) return target->bits > source->bits;

    // Unsigned into signed needs a bit to spare for the sign.
    return target->bits > source->bits;
}

static void defineIntegerTypes(Module& module) {
    GlobalBase global = *module.types;
    Array<TypePtr> types;

    struct Width { StringView name; U16 bits; bool isSigned; };
    static const Width widths[] = {
        { "I8"_v, 8, true },   { "U8"_v, 8, false },
        { "I16"_v, 16, true }, { "U16"_v, 16, false },
        { "I32"_v, 32, true }, { "U32"_v, 32, false },
        { "I64"_v, 64, true }, { "U64"_v, 64, false },
    };

    for(auto& width: widths) types.push(addInteger(module, width.name, width.bits, width.isSigned));

    // FromInt first, because Num declares it as a superclass: `1` has to mean something for a
    // type before `+` on it can be told what `x + 1` is.
    for(auto type: types) defineFromInt(module, type);

    for(auto type: types) {
        defineEq(module, type);
        defineOrd(module, type);
        defineNum(module, type);
        defineIntegral(module, type);
        defineTruth(module, type, emitTruthy);
    }

    // The conversion ladder, over these types and the two Core integer types they sit alongside.
    // Pairs that are both Core's already have their rung and are skipped rather than declared
    // twice, which would leave instance selection with two answers to one question.
    auto coreCount = types.size();
    types.push(module.scalar.int_);
    types.push(module.scalar.long_);

    for(Size from = 0; from < types.size(); from++) {
        for(Size to = 0; to < types.size(); to++) {
            if(from == to || (from >= coreCount && to >= coreCount)) continue;

            if(widens(global, types[from], types[to])) {
                defineConversion(module, "Widen"_v, "widen"_v, types[from], types[to]);
            } else {
                defineConversion(module, "Narrow"_v, "narrow"_v, types[from], types[to]);
            }
        }
    }
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
    attachIntrinsic(module, "sizeOf"_v, emitSizeOf);
    attachIntrinsic(module, "alignOf"_v, emitAlignOf);

    attachIntrinsic(module, "+"_v, emitPointerOffset<Value::Add>);
    attachIntrinsic(module, "-"_v, emitPointerOffset<Value::Sub>);
    attachIntrinsic(module, "difference"_v, emitDifference);

    attachIntrinsic(module, "=="_v, emitCompare<CompareOp::Eq>);
    attachIntrinsic(module, "!="_v, emitCompare<CompareOp::Ne>);
    attachIntrinsic(module, "<"_v, emitCompare<CompareOp::Lt>);
    attachIntrinsic(module, "<="_v, emitCompare<CompareOp::Le>);
    attachIntrinsic(module, ">"_v, emitCompare<CompareOp::Gt>);
    attachIntrinsic(module, ">="_v, emitCompare<CompareOp::Ge>);

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

    // The types have to exist before the signatures that name them are read, and the instances
    // before any body that uses one - which is the same order Core is built in. Core has to be
    // imported first of all, since the classes these instances join are its.
    resolveImports(*native, *nativeAst, nullptr);
    defineIntegerTypes(*native);

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
}
