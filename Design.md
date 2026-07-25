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

Compound types are implemented as tuples. The fields in a tuple can be either named or unnamed; if one field is named, all of them have to be.

Examples:
 - `{True, "Yes"}` is of type `{Bool, String}`
 - `{value = True, name = "Yes"}` is of type `{value: Bool, name: String}`

## Function types

Functions are first-class types and can store context data.

Examples:
 - `(a: Int) -> a * 2` is of type `(Int) -> Int`

## Record types

A record defines a new, distinct type that can be constructed. A record has one or more type constructors, where each constructor defines the type contents for that constructor.

Examples:
- `data Vec2 {x: Float, y: Float}` defines a record `Vec2` with a single constructor `Vec2` which consists of a tuple with two floats. A record with a single constructor can be used as if it is an instance of its contents.
- `data Lit = NumLit(Double) | StringLit(String) | BoolLit(Bool)` defines a record `Lit` with three constructors. Each constructor has its own associated data.
- `data Result(a) = Ok(a) | Err { code: Int, reason: String }` defines a record `Result` with two constructors. The `Ok` constructor contains a parametric type defined on instantiation. The `Err` constructor contains a tuple with error data. Note that `Err { code: Int, reason: String }` is equivalent to `Err({ code: Int, reason: String })`.

## Type aliases

An alias acts exactly like its target type. This is useful for things like naming a specific tuple or function type, instead of copying it to multiple places and possibly having to change them later.

Examples:
- `alias EventHandler = (Event, Date) -> Bool`
- `alias Annotated(a) = {a, String}`

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

## Typeclasses, higher kinded types, etc

Yana supports many type system features normally only found in pure functional languages. Most notably, we implement type classes similar to Haskell with GHC extensions, as well as allowing for more limited higher-kinded types (mainly to support some standard library types). It is important to remember that the language is intended to be _easy to learn_ - we do not want to have to use complicated functional programming terms just to explain how Hello World is implemented. As such, the use of these type system features will be fairly limited in the standard library, instead using a more procedural approach.

### Parsing

Parsing is handled in a fairly simple way. Any identifier inside a type is a concrete type if is starts with a capital letter, and an unknown type if not. Higher-order types are represented as type system-level function calls, and parsed in a similar way. We allow omitting parentheses for "type calls" in some cases to reduce syntax noise in the code.

### Resolving

Resolving is by far the most complicated stage. We need to do several things here:

 - Create generic contexts for all functions and type definitions. These contexts contain all unknown symbols known in that context.
 - For each context, resolve any explicit constraints that were declared. Constraints can include typeclass implementations, function signatures and more. Note that constraints are per-context rather than per-type - while this increases complexity, it is also required to be able to express constraints such as `(type, target, Serialize(type, target))`.
 - Recursively resolve constraint implementations where possible. This mainly means resolving referenced typeclasses and types.
 - Resolve functions using the resolved generic contexts. This encodes the generic operations into the IR at current resolve level. Each context is implicitly available inside its function, with instructions for performing operations defined by the constraints. This gives targets flexibility in how they actually generate code for these operations.
   - Whenever a value (generic or not) is sent to a generic argument slot in a function call, the special `CallGen` or `CallDynGen` instructions need to be used. These instructions include the creation of a runtime context corresponding to the called function's constraints. Each constraint needs to be satisfied. This context is then 'sent' (implementation defined) to the called function. Since the entry point of any program is a non-generic function, it follows that each generic function call has an explicitly defined generic context.
 - In many cases, calls to generic functions will be both slower and result in larger code size than fully-defined ones. For this reason, an additional pass is performed which specializes generic functions for explicitly defined contexts, and replaces any generic calls with normal ones.

### Native implementation

The native implementation will require a bit of type metadata, given that we do not want to compromise stored value size when used generically. More specifically, generic functions should never require the use of indirection in input data. We use the following high-level approach:
 
 - Generic functions include an extra implicit parameter, which is a pointer to the generic context with all required metadata.
 - Generic arguments are always passed by reference, preventing having to conditionally copy values based on their runtime size. This also means all functions implementing typeclass instances generate two code-level functions - one with the final required signature following normal conventions, and a stub with the generic reference-based signature. This stub will dereference to SSA registers where possible and call the real implementation, and can be optimized away in many cases.
 - The generic context includes type info about all unknown types visible in the context. This includes the size of the type, and a platform-specific store type. This allows performing structure reordering and limited value packing, while still supporting retrieving stored generic values.

### JS implementation

At present, the most straightforward implementation in JS would be to generate an array of operation handlers for each context in a `CallGen` instruction, merging any exactly equal contexts. Each constraint has a corresponding executor - typeclass constraints are pointers to a different context for that specific typeclass implementation, function constraints are function pointers, and field constraints are indices.

It remains to be tested whether this simple approach will give acceptable performance and final code size, after aggressive specialization to remove tables. If not, we could try more complex implementations such as adding context data to object prototypes where possible (mainly for type classes with a single argument, which is also the most common case).

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
Since the native target needs to be able to interface other native languages, we have to support raw pointers. This has the additional benefit of being able to use manual memory management for optimizations, although such code is obviously not portable between native and other targets.

Everything to do with pointers is hidden inside the `Native` module, which is mostly unsafe to use (we should probably add a compiler flag to disable importing it entirely). The value pointed to is always mutable, since the underlying memory is always mutable. This also simplifies the implementation, since immutable values are always referred to as SSA registers. The following types and functions are exported (among others):
- The generic `*` type. This defines a pointer-to type, such as `*Int`.
- Operators on the `*` type, such as `+`, `-`, `>`, `<`, `==`, etc.
- Pointer creation functions:
   - Unary operator `fn (a) *(it: *a) -> a`. Dereferences a pointer.
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
import Native

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




### Type System

- Single ownership. Values can be either borrowed, copied, or moved into a new scope. The default is immutable borrowing, but for the sake of convenience, POD (plain old data) structures, as well as any type implementing `TrivialCopy`, can be implicitly copied instead of borrowed/moved. For any type that needs to execute code (other than simple memory copies) to copy itself, all copies are explicit and immutably borrowing/moving is the default.
- Example semantics for functions (borrow/trivial copy is the default):
  - `fn borrow(a: String)` -- borrows 'a' immutably. Multiple immutable borrows can be created, but the object can't be modified while any are alive.
  - `fn copy(a: Int)` -- copies an immutable integer 'a' into the scope of the function, as `Int` is trivially copyable.
  - `fn borrow(&a: String)` -- borrows 'a' mutably. Only one mutable access point can exist at a time. Modifying `a` will modify the actual value located elsewhere.
  - `fn sink(->a: String)` -- takes mutable ownership of 'a'. It can only be accessed through the binding in this function afterwards.
  - `fn sink_copy(->a: Int)` -- copies a mutable integer 'a' into the scope of the function, as `Int` is trivially copyable. Modifying `a` only affects the local binding.
  - `fn ref(a: *String)` -- copies an immutable unowned reference to a mutable String 'a' into the scope of the function, as the type `*(a)` is a trivially copyable type. References are fat pointers that contain information to verify at runtime that the object it references is still alive.
  - `fn ref_mut(->a: *String)` -- copies a mutable unowned reference to a mutable String 'a' into the scope of the function. Since the reference itself is mutable, it can be reassigned to reference a different value.
  - `fn ptr(a: Ptr(String)` -- copies an immutable unowned raw pointer to a mutable String 'a' into the scope of the function. Raw pointers have no overhead, and also no guarantees.
  - `fn ptr_mut(->a: Ptr(String)` -- copies a mutable unowned raw pointer to a mutable String 'a' into the scope of the function. Since the pointer itself is mutable, it can be reassigned to point to a different value.
  - `fn init(set a: CustomType)` -- special case for initializer functions; takes an uninitialized instance of `CustomType`, and cannot return until every member has been set to a valid value. Works similar to a mutable borrow otherwise.
- Example semantics for types (move/trivial copy is the default):
  - `let t = { a: 42, b: "hello world" }` -- `a` is copied into `t`, while `b` is moved. Everything in `t` is immutable.
  - `let u = { &a: 42, &b: "hello world" }` 

- Classes used to manage ownership
  - `TrivialCopy` is implicitly implemented by any type where all members are `TrivialCopy`, and most primitive types (like `Int`, `Float`, `*(a)`, `Ptr(a)`, `[(a: TrivialCopy) * length]` but _not_ `String`).
  - `TrivialSink` is implicitly implemented by any type that doesn't contain any references to its own address. Currently this is done automatically for any type that has no references or pointers to its own type - this is not strictly safe, but the only way to make this unsafe is by doing tricks with raw pointers and allocation, which is already inherently unsafe. For types that can reference themselves, but are known to be movable, `Sink` can be added manually.  
  - `Sink` types implement `fn sink(set to: a, ->from: a)`, which takes an uninitialized instance of a type `a`, and initializes it from the moved source. It should make sure any internal data is moved over correctly (since that couldn't be done implicitly by `TrivialSink`).
  - `Copy` types implement `fn copy(from: a) -> a`, which performs a full copy of the instance and associated data (though not necessarily a deep copy). `copy()` needs to be called manually in cases where a value shouldn't/can't be moved.
  - `Drop` types implement `fn drop(->value: a)`, which is called when a live instance dies (goes out of scope). It is _not_ called on the original memory location after an instance is moved from.

### Important considerations

- Object lifetime is independent of any bindings referencing it; local bindings do not "contain" the object.
- An object's lifetime ends after the last time it is used (not when any binding that may exist goes out of scope).
  - RAII use cases where this would be an issue, such as `MutexLock(Mutex)`, are made explicit through linear types and `defer`.
  - Need to consider interactions between this and exception handling, if exceptions are ever added.
- This avoids issues like `auto x = ...` implicitly copying a returned reference, or `auto& x = ...` reading garbage memory if the return type ever changes to a non-reference type.
- Function argument types work for locals too:
  - `let x = 0` <- 0 is allocated locally, but is only borrowed by `x`. The underlying int is freed after the last use of `x`, unless it is re-borrowed, in which case it gets freed after the last use of both.
  - `let &y = 0` <- 0 is allocated locally, and is borrowed mutably by `y`. If we created another binding such as `let z = y`, then `y` would be inaccessible until `z` dies, unless we copy it: `let z = y.copy()`.
  - `let ->z = x` <- explicitly takes ownership from `x` over the value being bound. `x` stops existing after this point.
  - This is relevant particularly when used in combination with lenses and iterators - by default, you take an immutable borrow of the returned values (instead of implicitly copying), and the source value stays alive as long as any borrow exists. Using a `->` binding explicitly takes the value (if this is allowed by the yielded type).

### Random notes

- Functions can be overloaded on return type (through classes). Example:
  - ```
    class DecimalConversion(a, b):
      fn round(from: a) -> b
      fn floor(from: a) -> b
      fn ceil(from a) -> b
    
    instance DecimalConversion(F32, I32):
      ... implementations
    
    instance DecimalConversion(F32, I64):
      ... implementations
    ```
  - The specific instance called depends on the inferred return type (and potentially inferred source type). This could be a function argument (`fn f(value: Int)` can be called as `f(round(65.6))`). In cases where the return type isn't inferred, it can be specified explicitly: `round(65.5) :: U64`.
  - Inference for a given statement is done bottom-up, left-to-right. When evaluating the expression tree, type requirements are gathered and passed up (such as the coerce operator above). If no types in a function call have enough requirements to determine the correct overload, the arguments are evaluated left-to-right to their default types. If no concrete type is found for a given argument even then, inference fails - we don't evaluate the rest of the arguments and then backtrack to find a final resolution. This is somewhat limited, but also intended, as going overboard with inferred types will make code harder to read.
  - Inference for a function is done top-to-bottom. That is, the function arguments need to have known types, and the rest is inferred from there.

- Linear types
  - Make the destructor of a type explicit or private, which requires any function/type containing an instance to perform an action before going out of scope (such as explicitly calling close() or calling a function that takes an ->instance).
  - Example uses: mutexes (make unlocking explicit rather than implicit, especially if we define object lifetime by its last use); connection pools (forced to give back the connection after using it); various other cases where a specific action must not be forgotten to be executed.
- Lens functions
  - Like do notation in Haskell - perform a linear function call in code, but transform it into continuation passing during compilation. 
  - Implementing this correctly means that both the callee and called function can use the same stack, with no dynamic allocation or other inefficiencies. 
  - Since the code after the lens call is actually a callback internally, we can use normal single ownership + value semantics for the passed arguments without any additional complexity.
  - These have lots of use cases; for example, an http client request can be implemented as a lens, allowing it to allocate all its internal data on the stack without exposing it anywhere. It yields the final result, containing a `Reader` for the response body which can be read from without buffering the full response, and without worrying about the lifetimes of any data it uses.
- Iterators
  - Similar to lens functions, but can yield values multiple times, and support aborting. The implementation here needs some care - if done the same way as lenses, we can only have one "active" iterator at a time (since only the one at the top of the stack can be safely resumed). 
  - Another option would be to require iterators to have a statically determinable max stack size, so we can allocate them independently on the same outer stack without risking them overwriting each other.
  - A third option: implement iterators like lenses for simplicity, but if multiple iterators potentially overlap, one of them is allocated on a separate fiber stack owned by the caller. Note that "overlap" here means that they have intertwined usage - it's still safe to call an iterator within an outer iterator, as long as the inner iterator gets destroyed before the next iteration of the outer one.
  - Example uses: normal iterators (arrays, maps, etc), but also common functionality like string splitting/tokenizing (allows the caller to process each substring as an immutable borrow without copying or creating a full list result first).
- Checked references
  - Heap/stack allocations that are potentially captured into a reference contain an additional generation word based on a random seed before the actual data.
  - Checked references to objects (that is, outside the normal single ownership type system) are fat pointers, containing both the actual pointer, a potential offset within the allocation, and the value of the generation word at the time of capture.
  - Deallocating memory overwrites the generation word.
  - When accessing an object through a checked reference, the stored generation is compared to the current actual generation; a runtime error occurs if they don't match.
  - In debug mode, the generation word can instead be an atomic reference count - if any references exist upon deallocation, throw a runtime error. This should deterministically detect stale pointers in the vast majority of cases (but we can't guarantee that all combinations of state will be hit during debugging).
  - This is definitely not zero-cost, and wastes a lot of cache in pointer-heavy structures, but is a good and simple way to support safe, arbitrary references in less demanding parts of an application. The majority of references should still be owned ones in practice; where this is not the case, one of the other solutions provided by the language will likely work better.
  - Example use: global state object in a web server (shared between threads), which provides access to various keys, resources, etc.
  - This should work across threads and be faster than atomic reference counting, but we still need to test this.
  - Does this mean that we can't release virtual memory to the system? But we should at least be able to do `madvise(DONTNEED)` or `mmap(MAP_FIXED)` to release the physical memory, while retaining + reusing the virtual address space later.
  - Investigate pinning of objects - basically 1-bit reference counts (store the old value on the stack and restore it after finishing) that prevents it from being deallocated, so as long as the pin is alive, we can share and access a normal borrowed reference.
- Regions
  - Arena allocator that reserves a large amount of virtual memory, allowing for linear allocations.
  - Regional pointers only store a 32-bit offset within the region, saving lots of memory for pointer-heavy structures.
  - An instance of the region is required to access a regional pointer, so we automatically get memory safety as long as the region instance itself is safe.
  - All objects allocated in the region are alive for as long as the region is, so no need to keep track of lifetimes.
  - Region-allocated objects should be `TrivialCopy` or have reference semantics, otherwise it defeats the previous point by still having to keep track of lifetimes to call `drop()`. However, we could in principle have a stack of (pointer, drop) pairs in the region for any `Drop` objects allocated within it, which would be walked backwards when the region itself is dropped.
  - Regions themselves should likely have the same semantics as other types (multiple immutable borrow, singular mutable borrow, passed between functions).
  - Different regions (conceptually, from the application's perspective) should have different types in the type system, and regional pointers should be typed with them.
  - There needs to be a way to implicitly pass regions along the call stack, and use that passed region to implicitly dereference regional pointers - otherwise, the code using them becomes far too verbose.
  - There needs to be a way to use region objects as arguments to normal functions - otherwise, you get the same issue as with `async` in other languages. But depending on how flexible the standard library gets in the end, maybe normal typeclasses would be enough to implicitly generate a regional version of each function?
  - To be useful as a general concept, there will need to be multiple instances of the same region type. How do we prevent dereferencing a region pointer with the wrong region of the same type? Will it be enough to do this in debug mode only, as any wrong use that happens in release mode will almost certainly happen in debug mode too?
  - It will not be possible to turn a region pointer into a checked reference, but this shouldn't be much of an issue - there would be basically no common functions that take such a reference as input, since everything is built around single ownership.
  - In general, we need to take great care that it is not possible (without clearly being marked as unsafe) to convert a region pointer to something that can outlive the region it belonged to - but without making region objects completely impossible to use together with normal functionality.
  - This feature, if done right, would be a real USP - we get better performance, smaller working set, and simpler resource management, with hopefully very little added complexity. Actual memory usage depends on the specific use case - we save memory by using smaller pointers and no allocation tracking overhead, but objects may also stay alive for longer than is strictly needed.
  - Example uses:
    - Almost any operation or series of operations that allocates many small, simple objects will benefit a lot from this. 
    - An http client could allocate all response-related data into a small region tied to the response object. 
    - A server could have a region for all data tied to a specific request, ensuring any request handler can freely allocate small objects into the region, store them into other (regional) data structures, return them, etc - since the region isn't destroyed until the request handler has fully completed. We don't have to worry about regional pointers escaping, since there is no way to dereference them without access to the region (but again, need to somehow ensure that it is the _right_ instance of that region).
    - Compiler: separate regions for parsed AST, generated IR, code generation.
    - Parsing: parse complex objects into a small region (also limiting the max memory used, which is a _good_ thing when parsing untrusted data), returning a (region, parsed data) pair. It should also be possible to send in an existing region of a different type here.
- Snapshot GC
  - Useful in cases where use of data is temporal (requests in a server, frames in a game).
  - Keep a snapshot instance on the stack that avoids shared data from being deallocated.
  - Queue deallocations together with the time they were deleted at; run them once the last snapshot referencing that time is gone.
  - Shapshot implementation has very little overhead (certainly compared to the alternatives); almost entirely done through thread-local data with only reads of shared data (except when locking to perform a GC cycle). 
  - Relatively high cost for deallocation (at least compared to the other two techniques above) - use it for infrequently created data shared between threads, where normal borrowing is infeasible.
