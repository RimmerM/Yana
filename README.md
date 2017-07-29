# Expressions

- typedexpr: infixexpr `as` type. Forces an expression to have a certain type, used for return type polymorphism. Does not cast existing types.
- infixexpr: prefixexpr [op infixexpr]. Applies an operator function to two expressions.
- prefixexpr: [op] leftexpr. Applies an operator function to one expression.
- leftexpr: 
   - `let` [`&`] varid [`=` expr]. Declare one or more variables (acts as a block).
   - `match` expr`:` alts. Pattern match on a value.
   - `if` expr `then` expr [`else` expr]. If expression or statement depending on whether the return value is used.
   - `if:` cases. Multi-way if expression, used instead of repeating if-then-else.
   - `while` expr`:` exprseq. Loop while a condition holds.
   - `return` expr. Return from the function with the provided result.
   - appexpr.
- appexpr: baseexpr{`(`arg, ...`)`, `.`selexpr}. Call a function or field.
- baseexpr:
  - `{`expr, ...`}`. Tuple construction.
  - `{`varid `=` expr, ...`}`. Tuple construction, named fields.
  - `{`varid `|` varid `=` expr, ...`}`. Tuple construction, from existing value.
  - `[`expr, ...`]`. Array construction.
- selexpr:
  - literal. Construct value from constant.
  - varid. Read variable binding.
  - `(`expr`)`. Nested expression.

# Type system

All type names start with a capital letter.

## Basic types

- Bool
- Int
- Long
- Float
- Double
- String

Integer types can have different sizes when stored in memory. However, when loaded they are converted to the closest primitive integer in size. The normal integer operators are signed; however, unsigned operators are available. Unsigned types can be implemented as library code without performance loss.

Strings are collections of zero or more characters. Each character in a string consists of one or more code points. The type of a code point depends on the current platform - differences between the default for each target make it impractical to enforce a single string encoding. Strings can be converted to arrays of a particular code point type if a specific encoding is needed.

Examples:
- `False` is of type `Bool`
- `0` is of type `Int`
- `0l` is of type `Long`
- `0f` and `0.0` are of type `Float`
- `0d` and `0.0d` are of type `Double`
- `"Hello world"` is of type `String`

## Collection types

Arrays `[]` store zero or more values of one specific type.
Maps `[=>]` store zero or more key-value associations of a specific key and value type.

Examples:
 - `[0, 1, 2]` is of type `[Int]`
 - `["Hello" => 1, "World" => 2]` is of type `[String => Int]`

## Compound types

Compound types are implemented through tuples. The fields in a tuple can be either named or unnamed; if one field is named, all of them have to be.

Examples:
 - `{True, "Yes"}` is of type `{Bool, String}`
 - `{value = True, name = "Yes"}` is of type `{value: Bool, name: String}`

## Function types

Functions are first-class types.

Examples:
 - `(a: Int) -> a * 2` is of type `(Int) -> Int`

## Record types

A record defines a new, distinct type that can be constructed. A record has one or more type constructors, where each constructor defines the type contents for that constructor.

Examples:
- `data Vec2 {x: Float, y: Float}` defines a record `Vec2` with a single constructor `Vec2` which consists of a tuple with two floats. A record with a single constructor can be used as if it is an instance of its contents.
- `data Lit = NumLit(Double) | StringLit(String) | BoolLit(Bool)` defines a record `Lit` with three constructors. Each constructor has its own associated data.

## Type aliases

An alias acts exactly like its target type. This is useful for things like naming a specific tuple or function type, instead of copying it to multiple places and possibly having to change them later.

Examples:
- `alias EventHandler = (Event, Date) -> Bool`

## Mutability

- Immutable by default
- Full control over mutability for everything gets complex very quickly since there are so many levels - variables, the contents of variables, fields in those contents, etc.
- Variables are always immutable in that their binding cannot change.
- There are three possible value types:
   - Default: The value is immutable.
   - `&`: The value acts as a reference. There can be multiple variables referencing the same value. Any contents inside can be changed.
   - `*`: The value is "flattened" into its containing context. It acts as a value type where all contents are copied shallowly when it is assigned to or from.
- Examples:

```
-- This value is immutable. Neither the variable nor the fields can be changed.
-- x: {x: Int, y: Int}
let x = {x = 0, y = 0}

-- This value is mutable and is copied as a reference. 
-- Assigning this to a different variable and changing it 
-- will affect the original value.
let &x = {x = 0, y = 0}
    &y = x
    z = x
x.x = 1
x.y = 2 -- y.y is now also 2, but z.y is still 0.

-- This value is immutable but contains a reference.
-- The reference is immutable, but its contents can be changed.
-- x: {x: Int, &y: Int}
let x = {x = 0, &y = 0}
x.y = 1
```

- Problem:
```
-- Normal case: use a reference as a mutable variable.
let &x = 0
while x < 10:
  x = x + 1

-- If we update a reference value through assigment, 
-- then that should work for all references. 
-- We don't separate primitive types from compound ones.
let &y = {a = 0, b = True}
while y.a < 10:
  y = {y | a = y.a + 1}

-- Now we create a mutable tuple with a mutable reference to y: 
let &z = {a = 0, &b = y}

-- Here we have a problem. Should the assignment change the reference in z, 
-- or should it assign to the contents of y?
z.b = {a = 0, b = False}
```

## Indirection

By default, the compiler chooses how a value is stored - it can either store a reference to the heap or the value itself. It is also possible to manually set the storage class:


# Implementation

## Overloading

Overloading based on namespaces and type classes only. This supports most cases where overloading is useful:
 - Multiple unrelated types with completely unrelated methods that happen to have the same name. This is supported because type methods are namespaced. When called through the `type.method` syntax we know the namespace through the target type. When called as the normal function, the `Type.method(type)` syntax must be used to indicate the function to call.
 - Multiple related types that support a similar operation: supported through type classes.
 - TODO: Look into supporting splitting function names interleaved between parameters, like ObjC. This would support most remaining use cases of overloading.

## Generics

- We don't want to have to generate a new version of each function that uses a generic type.
- We *really* don't want to forcibly use references in generic containers.

Possible solutions:
 - Send type info to generic functions. Size of each generic type to be able to handle them without knowing anything else.

Remaining design
- There are still quite a few remaining questions regarding the design of generics, since we want to follow the first two constraints. We want the default case to be some form of type erasure, in order to support using generics inside non-generic contexts. However, the design must support specialization when useful.

Sample cases:
```
-- This function has no defined 
fn (a) ()
```
## Convenience feature ideas
 - `class Default(a): fn default() -> a`
   - When constructing a type, any fields that implement Default get that value if not provided.
   - Allows to write code like
   ```
   instance Default(Float):
     fn default(): 0f

   type Float3 = {x: Float, y: Float, z: Float}
   let unit = Float3()
   ```
- Partial application:
   ```
   data Id = Long
   data User = {
     id: Id,
     name: String,
     ...
   }

   let *userCache: [Id => User] = [=>]

   fn (a) update(id: Id, cache: [Id => a], update: (a) -> a):
     cache.(id) >>= update >>= Map.set(cache, id, _)
   ```
- Nested updates:
    ```
    type Vec2 = {x: Float, y: Float}
    type Rect = {origin: Vec2, size: Vec2}

    fn translateX(it: Rect, by: Float) = {it | origin.x = origin.x + by}
    ```

## JS target

- JS doesn't support value types, so copying values creates new objects.
- Compiler tries to optimize in the other direction - replace values with references where possible.
- Use immutable types to avoid the compiler having to copy objects.
- Use explicit references for mutable types

## Native target

- LLVM for code generation and low-level optimizations
- LLVM-like IR aware of memory model for high-level optimizations, such as removing allocations.
- Only target that supports raw pointers; important for interfacing C.
- Needs to be possible to get the address of a GC-object in some cases, to avoid using unsafe pointers everywhere when calling OS apis. This should be safe as long as these pointers aren't returned or stored on the heap, since the reference they were taken from still exists on the stack.

- Strings: one pointer to a variable-length structure. First field is the length, contents inline after that. Should we add a zero byte last to make interfacing C easier?
- Arrays: depends on mutability. Immutable arrays can have the same layout as strings, but mutable ones need to have a separate pointer to the data.
- Maps: immutable maps can be a single pointer to the length, then an array of keys, then an array of values. Mutable ones need a separate structure with pointers to a key and a value array. Small maps can just search the keys array, while large ones act as hash maps.

### Pointers
Since the native target needs to be able to interface other native languages, we have to support raw pointers. This has the additional benefit of being able to use manual memory management for optimizations, although such code is obviously not portable.

Everything to do with pointers is hidden inside the `Unsafe` module, which, as the name suggests, is always unsafe to use (we should probably add a compiler flag to disable importing it entirely). The value pointed to is always mutable, since the underlying memory is always mutable. The following types and functions are exported (among others):
- The generic `*` type. This defines a pointer-to type, such as `*Int`.
- Operators on the `*` type, such as `+`, `-`, `>`, `<`, `==`, etc.
- Pointer creation functions:
   - Unary operator `fn (a) *(it: *a) -> a`. Derefences a pointer.
   - `fn (a) addressOf(it: a) -> *a`. Takes the address of a value.
- `fn (a) sizeOf(it: a) -> Int`. Returns the size in memory of a specific type.
- `fn allocate(type: TypeInfo) -> *U8`. Allocates an instance of a specific type.
- `fn (a, b) cast(it: *a) -> *b`. Casts a pointer of one type to a different one.
- `fn (a, b: Integral) asPtr(it: b) -> *a`. Casts an integer to a pointer with the same contents.
- `fn (a, b: Integral) asInt(it: *a) -> b`. Casts a pointer to an integer with the same contents.
- `fn (a) Array.values(self) -> *a`. Returns a pointer to the first value in an array.

### Interfacing the OS
Obviously, the native target needs to call the outside world in order to be useful. The standard library can implement most common operations by using syscalls directly; this avoids linking in the large libc. The best way to do this would be adding a system call intrinsic; then, the OS calls can be implemented through a template using that. Example:
```
import Unsafe

-- The intrinsic declaration.
foreign import fn syscall(a: *U8, b: *U8, c: *U8, d: *U8, e: *U8, f: *U8) -> *U8

-- An example system call.
fn write(fd: Int, buffer: *U8, count: Int) -> Int:
    syscall(asPtr(fd), buffer, asPtr(count)) |> asInt
```

### GC
Complicated, difficult to make fast. See Go discussions for trouble with pausing threads when compiling AOT; you can't dynamically change the running code like JVM and CLR do.
- Look into the possibility of pseudo-shadow stack: Store all GC references in a predictable location for each stack frame. Null on function entry. Now we can forcibly pause any thread anywhere except for two special cases:
  - On function entry before reference area is initialized. On way to solve this could be storing the number of references for each function, then compare rsp with rbp to check if the function has started running yet. A better way could be to write an additional 0 word to the stack *before* calling the function - this value can be updated as references are initialized. Need to check if doing so is faster than initializing with zeroes.
  - On function return before return value is written to reference area. This could be solved by writing references into parent stack frame before returning (used for returning large values anyway).
  - Since these solutions require quite precise control over the generated code, it may be difficult to do in LLVM.
- Arrays need special GC support. Only aggregate primitive type, so everything is based on them. We could store a number of GC flag bits in the object header instead of just one, to support a granular write barrier (instead of having to look through the while array after each change).
- Like the stack, we can group all references in aggregate types and store them at the start (only con is a possible 8-byte padding when a type contains both references and SIMD values). We then store the number of references in the GC object header, to avoid having to write type info.
