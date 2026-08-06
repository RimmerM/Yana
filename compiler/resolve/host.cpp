#include "host.h"
#include "intrinsic.h"
#include "name.h"
#include "../parse/parser.h"

/*
 * The host operations - Implementation-Containers.md §14.1.
 *
 * Two shapes, and the difference between them is the whole of what a host node had to be able to
 * say:
 *
 *   A **member** operation - `length`, `splice` - is an `InstNative` carrying the member's name. The
 *   emitter prints the name and knows nothing else about it, which is what Analysis-JS.md §2.4 asks
 *   for when it rules host knowledge out of codegen: a declaration below says `.splice` and the
 *   backend says nothing.
 *
 *   An **element** operation - reading, writing and borrowing `a[i]` - is not a node at all. It is a
 *   `ProjectionKind::Index` over a place rooted in the array reference, which is the same projection
 *   `[T *n]` introduced (§6) and which §14.1 expected to be a second node. A place is an lvalue, so
 *   one form gives the read, the write and the borrow together where an operation would have given
 *   only the read - and, more importantly, it puts a host element in exactly the position a native
 *   one is in for every pass above the backend. `store(p + i, v)` natively and `hostWrite(p, i, v)`
 *   here are both an assignment through a place rooted in a raw pointer, so the ownership passes see
 *   one shape rather than two and `Array(a)`'s teardown is one rule rather than two.
 */

static const char* kHostSource = R"HOST(
import Native

{-
   The host array.

   Every one of these is `@platform(js)`, so on a native build this module declares nothing at all -
   which is what makes a host operation unreachable rather than merely unused there.

   `%a` is the array, and what a `%a` means on this target is in host.h: an opaque reference to
   storage, since nothing this target can express does arithmetic on one. That is why these need no
   type of their own. A `HostArray(a)` newtype was written first and removed: it typed exactly the
   same programs, and it put a host element in a *record* place rather than a pointer one, which
   quietly moved every element of every container inside the ownership graph and made a write through
   one owe a drop the native path does not owe.
-}

-- An empty host array - `[]`. The array literal builds its own with the elements in it, since a
-- literal is one node rather than a call plus n pushes; this is what `emptyArray` is.
@platform(js) pub fn hostArray() -> %a

{-
   There is deliberately no `hostPush`, and the reason is ownership rather than taste.

   A host operation's arguments are *uses*: the ownership passes read a move out of an `InstMove` or
   out of an assignment into a place, and an operand of an `InstNative` is neither. So
   `self.push(item)` appends the value and leaves this frame still owning it, and the frame releases
   it at the end of the block - an element the array is holding, released while it holds it. It was
   written, and it read as if it worked, and the counter in `HostArray.yana` is what caught it.

   Appending is `arr[arr.length] = item` instead, which is `hostWrite` below and is an assignment
   through a place. That is the *same* hand-over the native `store(self.run.items + count, item)` is,
   read by the same pass, which is the property worth having: a container's two bodies differ in how
   an element is reached and in nothing about who owns it.
-}

-- `self.length`. A `Size`, which on this target is `Int` - and a host array's length is a `uint32`
-- by specification, so there is nothing wider to describe (§4.4).
@platform(js) pub fn hostLength(self: %a) -> Size

-- `self.splice(index, count)` - removing a range and closing the gap, which is what the host does
-- instead of the `copyMemory` the native side writes.
@platform(js) pub fn hostSplice(self: %a, index: Size, count: Size) -> {}

{-
   The element - `self[index]`, in the four positions an element is reached from.

   `hostRead` answers the element *by value*, which is what makes `let ->doomed = hostRead(xs, i)` a
   move out of the array: it is the same line `*(self.run.items + i)` is on the native side, and it
   is what a container's teardown and its `remove` are written with.

   `hostAt` and `hostAtMut` answer a borrow, and carry the `return` marker for the reason `borrow`
   does: the result names storage this function was handed rather than storage of its own.
-}
@platform(js) pub fn hostRead(self: %a, index: Size) -> a
@platform(js) pub fn hostWrite(self: %a, index: Size, value: a) -> {}
@platform(js) pub fn hostAt(return self: %a, index: Size) -> &a
@platform(js) pub fn hostAtMut(return self: %a, index: Size) -> &a

{-
   The host string - Implementation-String.md part 2's JS column.

   `String` here is the host `string` primitive with no wrapper at all, which is what part 2 asks for
   and what makes a Yana string free to hand to any host API. So every operation below is the host's
   own, and none of them allocates anything Yana has to account for: the collector owns every string
   these produce, which is why `Reclaim(String)` has no JS instance.

   The two that are *not* here are the two a program would expect most. There is no `hostSubstring`
   and no `hostIndexOf`, because slicing and search are parts 4 and 5 of that document and this
   change is parts 2, 3 and 8 - the raw tier and concatenation. Adding either is a declaration here
   and a `HostMember` entry, and nothing else.
-}

-- `self.length`, in UTF-16 code units, which part 3 is explicit is *not* the same number the native
-- build answers for the same content. Both are O(1), and that is the property required to be
-- uniform rather than the value.
@platform(js) pub fn hostStringLength(self: String) -> Size

-- `self.charCodeAt(index)` - one raw UTF-16 unit, with no decode and no validation that it is a
-- whole code point. The exact counterpart of the native build's byte read.
@platform(js) pub fn hostCharCodeAt(self: String, index: Size) -> Int

{-
   Concatenation and comparison, as the host's own operators - see NativeOp::HostBinary.

   `<` on two host strings is raw UTF-16 code-unit order, which is exactly the default `Ord(String)`
   part 3 specifies, wrinkle included: it can disagree with true code-point order for
   supplementary-plane characters, and part 3 says so and points anyone who needs otherwise at an
   explicit `compareByCodePoint`. Getting that for free is the argument for the operator node.
-}
@platform(js) pub fn hostConcat(self: String, other: String) -> String
@platform(js) pub fn hostStringEq(self: String, other: String) -> Bool
@platform(js) pub fn hostStringLt(self: String, other: String) -> Bool

-- `String.fromCharCode(unit)` - one UTF-16 unit as a one-unit string, which is what appending a unit
-- to a host string has to go through. A call on the host's global rather than on a value, which is
-- why it is the one `HostGlobalCall` here.
@platform(js) pub fn hostFromCharCode(unit: Int) -> String

-- `console.log(text)` - the host's own output, which is what `print` is there. A call on a global
-- rather than on a value, like `String.fromCharCode` above.
@platform(js) pub fn hostLog(text: String) -> {}

{-
   `throw "..."` - how a program stops here, and the one thing every host agrees means "stop".

   There is no `abort` in JavaScript and no exit status a script can set; an exception nobody catches
   ends the program and reports what it carried, which is exactly what a failed check needs to do.

   No argument, and the message is the emitter's. A string literal written here would need `Text`,
   which is built after this module and imports it - so the one thing this declaration could not
   carry is the sentence it exists to print.
-}
@platform(js) pub fn hostFail() -> {}
)HOST";

namespace {

/*
 * A member operation.
 *
 * The name is a template parameter rather than a field of a table, because `Intrinsic` is a plain
 * function pointer with nothing to capture in - which is the same reason `emitBinary` is a template
 * over its `Value::Kind`. `HostMember` names the members this module has; adding one is a line here
 * and a declaration above.
 */
enum class HostMember: U8 {
    Length,
    Splice,
    CharCodeAt,

    // The operators, whose "member name" is the operator's own spelling - see NativeOp::HostBinary.
    // They are in the same enum because they are read the same way: `method` carries the text and
    // the emitter prints it, and which arm prints it as a member and which as an operator is the
    // `NativeOp` rather than anything here.
    Concat,
    Equal,
    Less,

    // A dotted path on the host's global scope rather than a member of anything - see
    // NativeOp::HostGlobalCall.
    FromCharCode,
    Log,

    // The one that is a statement rather than a call or an operator - see NativeOp::HostThrow. Its
    // "member name" is never printed; the emitter writes `throw` itself.
    Fail,
};

StringView hostMemberName(HostMember member) {
    switch(member) {
        case HostMember::Length: return "length"_v;
        case HostMember::Splice: return "splice"_v;
        case HostMember::CharCodeAt: return "charCodeAt"_v;
        case HostMember::Concat: return "+"_v;
        case HostMember::Equal: return "==="_v;
        case HostMember::Less: return "<"_v;
        case HostMember::FromCharCode: return "String.fromCharCode"_v;
        case HostMember::Log: return "console.log"_v;
        case HostMember::Fail: return "throw"_v;
    }

    return "length"_v;
}

template<NativeOp op, HostMember member>
static ModulePtr<Value> emitHostMember(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId name) {
    auto text = hostMemberName(member);
    auto instruction = resolver.create<InstNative>(source, name, type, op,
                                                   resolver.context.addUnqualifiedName(text.ptr, text.length));

    for(auto arg: args) instruction->args.push(resolver.module.arena, arg);

    resolver.append(instruction);
    return isUnit(resolver.global, type) ? nullptr : resolver.ref(instruction);
}

// `[]`. The variadic form - a literal with its elements already in it - is built by `resolveArray`
// rather than declared, because a literal has an arity per literal and a declaration has one arity.
static ModulePtr<Value> emitHostArray(ExprResolver& resolver, Buffer<ModulePtr<Value>>, TypePtr type,
                                      LocationId source, StringId name) {
    auto instruction = resolver.create<InstNative>(source, name, type, NativeOp::HostArray);
    resolver.append(instruction);

    return resolver.ref(instruction);
}

/*
 * The element, as a place.
 *
 * Rooted at the array reference exactly as `*(p + i)` is rooted at `p`, so everything above the
 * backend - the borrow checker, the drop pass, the escape analysis - sees the shape it already knows
 * and none of them needed a case for a host container.
 */
static Place hostElement(ExprResolver& resolver, Buffer<ModulePtr<Value>> args) {
    return resolver.project(Place::atPointer(args[0]), ProjectionKind::Index, 0, args[1]);
}

static ModulePtr<Value> emitHostRead(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                     LocationId source, StringId name) {
    return resolver.load(hostElement(resolver, args), source, name);
}

// An assignment and not an initialization, for the reason `store` is one: what a raw pointer names
// is memory the program manages itself, and a pointer root is outside the ownership graph, so
// nothing is dropped either way.
static ModulePtr<Value> emitHostWrite(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                      LocationId source, StringId) {
    resolver.assign(hostElement(resolver, args), args[2], source);
    return nullptr;
}

template<bool mut>
static ModulePtr<Value> emitHostAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId name) {
    return resolver.ref(resolver.emit<InstBorrow>(source, name, type, hostElement(resolver, args), mut));
}

} // namespace

Place hostElementPlace(ExprResolver& resolver, ModulePtr<Value> array, ModulePtr<Value> index) {
    ModulePtr<Value> args[] = { array, index };
    return hostElement(resolver, { args, 2 });
}

ModulePtr<Value> emitHostLengthOf(ExprResolver& resolver, ModulePtr<Value> array, TypePtr type,
                                  LocationId source, StringId name) {
    return emitHostMember<NativeOp::HostField, HostMember::Length>(resolver, { &array, 1 }, type,
                                                                   source, name);
}

namespace {

static ast::Module* parseHost(Context& context) {
    auto id = context.addQualifiedName("Host", 4);
    Lexer lexer(context, context.diagnostics, StringView { kHostSource, stringLength(kHostSource) }, id);
    Parser parser(context, lexer, id);
    parser.allowSignatures = true;

    return new ast::Module(parser.parseModule());
}

} // namespace

void defineHost(Program& program) {
    auto& context = program.context;

    auto ast = parseHost(context);
    auto module = program.addModule(ast->name, *ast->region);
    program.embeddedAsts.push(ast);
    program.host = module;

    resolveImports(*module, *ast, nullptr);
    resolveModuleDecls(*module, *ast, nullptr, true);

    /*
     * Only where the declarations exist.
     *
     * Every one of them is `@platform(js)`, so on a native build this module read no declarations
     * and there is nothing to attach a hook to - which `attachIntrinsic` would report as an internal
     * error rather than skip, and rightly: a missing declaration is normally a typo.
     */
    if(!isJsMode(context.settings.mode)) return;

    attachIntrinsic(*module, "hostArray"_v, emitHostArray);
    attachIntrinsic(*module, "hostLength"_v, emitHostMember<NativeOp::HostField, HostMember::Length>);
    attachIntrinsic(*module, "hostSplice"_v, emitHostMember<NativeOp::HostCall, HostMember::Splice>);

    attachIntrinsic(*module, "hostRead"_v, emitHostRead);
    attachIntrinsic(*module, "hostWrite"_v, emitHostWrite);
    attachIntrinsic(*module, "hostAt"_v, emitHostAt<false>);
    attachIntrinsic(*module, "hostAtMut"_v, emitHostAt<true>);

    // The host string - Implementation-String.md part 2's JS column. `length` is a field and
    // `charCodeAt` a method, exactly as they are for an array; the last three are operators.
    attachIntrinsic(*module, "hostStringLength"_v, emitHostMember<NativeOp::HostField, HostMember::Length>);
    attachIntrinsic(*module, "hostCharCodeAt"_v, emitHostMember<NativeOp::HostCall, HostMember::CharCodeAt>);
    attachIntrinsic(*module, "hostConcat"_v, emitHostMember<NativeOp::HostBinary, HostMember::Concat>);
    attachIntrinsic(*module, "hostStringEq"_v, emitHostMember<NativeOp::HostBinary, HostMember::Equal>);
    attachIntrinsic(*module, "hostStringLt"_v, emitHostMember<NativeOp::HostBinary, HostMember::Less>);
    attachIntrinsic(*module, "hostFromCharCode"_v, emitHostMember<NativeOp::HostGlobalCall, HostMember::FromCharCode>);
    attachIntrinsic(*module, "hostLog"_v, emitHostMember<NativeOp::HostGlobalCall, HostMember::Log>);
    attachIntrinsic(*module, "hostFail"_v, emitHostMember<NativeOp::HostThrow, HostMember::Fail>);

    /*
     * `hostAtMut` answers a *mutable* borrow, and the grammar has one spelling for a borrow type -
     * so which of the two it is comes from the signature it appears in, and this one has no `return`
     * group to say so. Said here instead, exactly as `Native` says it about `borrowMut`.
     */
    if(auto found = module->functions.get(context.addUnqualifiedName("hostAtMut", 9))) {
        auto function = (*module->arena)[found.unwrap()];
        auto declared = (BorrowType*)(*module->types)[function->returnType];
        function->returnType = resolveBorrowType(*module, declared->to, true);
    }
}
