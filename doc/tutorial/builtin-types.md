# Builtin types

Yana uses a static, strong type system. This means that every value in a program has a single, well-defined type, and they are not implicitly converted to any different one. However, Yana also supports limited type inference, which means that the compiler can automatically define the type of something for you in many cases.

## Scalar types

* **Integers**. An integer is a natural number of a specific size, resulting in an upper and lower bound. The following integer types are provided:
   * `Int` is a 32-bit signed integer.
   * `UInt` is a 32-bit unsigned integer.
   * `Long` is a 64-bit signed integer.
   * `ULong` is a 64-bit unsigned integer.

  All operations on integers are performed on one of these types. However, for storage, integer types can also be annotated with a specific size in bits for interoperation with native languages and reducing memory usage.

  Integer literals are either normal numbers `1234`, hexadecimal numbers `0xff01`, octal numbers `0o777` or binary numbers `0b11001`.

  By default, unless a different type was inferred, integer literals get the type `Int`, or `Long` if the value is too large for an `Int`. The type can also be set by explicitly constructing it `let x = UInt(1) + UInt(2)` or a type coercion `let x = 1 + 2 :: UInt`.

* **Floating point numbers**. A floating point number uses a fixed-sized mantissa and exponent to be able to store large numbers with decreasing precision. The following types are provided:
   * `Float` is a 32-bit float.
   * `Double` is a 64-bit float.

  Floating point literals are decimal numbers with the syntax `45.067`, and can include an exponent `11.5e5`.

  By default, unless a different type was inferred, floating point literals get the type `Double`. The type can also be set by explicitly constructing it `let x = Float(1.1) + Float(2.2)` or a type coercion `let x = 1.1 + 2.2 :: Float`.

  Note that on some targets, all floating point operations may be performed with the highest precision if there is no difference in storage and performance.

* **Bool**. The boolean type `Bool` is either equal to `true` or `false`. A `Bool` acts exactly like a 1-bit integer, and all integer operations are also defined for booleans. All conditions, such as `if` or `while`, need to be of boolean type.

* The unit type `{}`. This type contains nothing, and is the type of i.e. a function with no return value.

## Strings

The `String` type contains a sequence of characters. Strings differ from arrays in that their contents have no defined type. This allows strings to be used independently from the encoding of their contents. Different targets use the encoding that is most efficient for that platform.

### String literals

String literals are text enclosed by `"` characters. They work mostly the same as in any C-like language regarding escape characters and gaps.

```yana
"A normal string"
"A \"string\" with some escaped characters.\n"

"A multi-line string \
 by using gaps. \
 In the compiled program, \
 this becomes a single line."

"A string \xfa5b with some indexed \o5070 code points. \38639"
```

### String interpolation

There are many cases where you want to format a string in some way. This can be placing a string value in between two literals, formatting a number with some suffix, or many other things. For these operations, Yana supports string interpolation. This allows you to place code inside of strings, and the resulting value is automatically converted to a string and inserted in the result.
An interpolated expression starts with `{` and ends with `}`.

```yana
let x = 5000
println("There are {x} elements currently in flight")
println("Tomorrow, there will be {x * 2 + 2000} elements in flight.")
println("Obviously, having \{ start an interpolation expression means that \{ needs to be escaped when used normally in a string.")
```

## Arrays

Arrays are dynamically sized lists of a certain content type. Each value inside the array has the same type.
If an array was created mutable, it can be resized by adding more items. The standard library contains many operations on arrays, supporting both a functional and a procedural style. In this example, we only look at array creation.

```yana
-- `primes` has the type [Int] and length 6.
let primes = [2, 3, 5, 7, 11, 13]

-- `strings` has the type [String] and length 3.
let strings = ["Hello", "World", "!"]

-- `even` has the type [{ value: Int, even: Bool }] and length 2.
let even = [{ value = 1, even = false }, { value = 10, even = true }]

-- `empty` has no content. Since there is no way to infer what should be inside it,
-- we have to to provide the type ourselves.
let empty = [] :: [Bool]
```

## Maps

Maps are associations from a set of keys to a set of values. Each key and value contained have the same type.
They can be compared with tables, objects, or map-like types from other languages.

```yana
-- `users` maps from a username to their age. It has the type [String -> Int].
let users = [
  "Will" -> 34,
  "Eva" -> 65,
  "John" -> 18
]

-- Print the age of John if we have it.
if let Just(age) = users."John":
  println("John is {age} years old.")
```