# Custom types

In addition to the built in types shown before, Yana provides several ways to create your own data types.

## Tuples

Tuples are a way to create a single value with multiple fields inside.
They are created using braces `{}`, and creating a tuple with two `Int` fields `{ 1, 2 }` results in the type `{ Int, Int }`.
Tuples don't need to be defined or declared anywhere - they are simply created whenever they are used.

A tuple contains one or more fields, similar to a `struct` in C or an object literal in Javascript.
Fields can be either anonymous or have their own name. However, if any field in a tuple has a name, then all must have one.

```yana
-- This creates a tuple of type { Int, Int }.
-- It has two fields, both of which are integers.
let &origin = { 0, 0 }

-- Unnamed fields can be extracted from tuples by their index.
let x = origin.0

-- We can also update fields the same way.
origin.0 = 1

-- This creates a tuple of type { x: Int, y: Int }.
-- It has the same contents as `origin`, but the fields have their own names.
let position = { x = 3, y = 5 }

-- Since `position` was created immutable, we cannot change any fields.
-- Instead, we can use an update expression to create a new tuple based on it.
-- In an update expression, each field from the source is copied
-- unless we provide a new value for it.
let position2 = { position | y = 6 }

-- `y` is now 6.
let y = position2.y
```

## Aliases

Sometimes you may want to use a specific tuple or array type in multiple places.
For example, you may have two functions that should always take the same arguments.
In these cases, you can create an `alias` to give a new name to an existing type.
This ensures that any uses always refer to the same type, and you don't have to write the full type each time.
Aliases act exactly like the type they refer to, and thus are not distinct types.

```yana
-- Here create a name for a Scalar type.
-- Using this name everywhere allows us to change the coordinate type we use at any time.
alias Scalar = Int

-- Here we give a name to a tuple with scalar `x` and `y` coordinates.
alias Pos = { x: Scalar, y: Scalar }

-- We can now create positions by simply using the name,
-- rather than explicitly setting the fields each time.
-- The integer literals here are automatically converted to whatever `Scalar` refers to.
let pos = Pos(5, 6)

-- We can still use the same tuple operations as before.
let y = pos.y
let nextPos = { pos | x = 0 }
```

## Records

While aliases allow you to give a name to a tuple with specific fields, there are also cases where you want to create a completely new type that is distinct from everything else. Using a record allows you to do this, while also supporting many other uses.

### Use as a single value

The simplest case of a record is to use it as a wrapper around some other type. This makes it a distinct type which is incompatible with the type it contains. You could compare it to defining a `struct` in C or a `class` with just fields in Java.

```yana
-- Here we define a record which simply wraps its contained type.
data Timestamp(Long)

-- If we want to create a Timestamp, we now have to explicitly construct it.
let zero = Timestamp(0)

-- You can also define a record to contain a tuple.
-- This makes it act just like a `struct`.
data User { id: Long, name: String, displayName: String, bio: String }

-- Records that contain a tuple can be constructed with separate fields,
-- or from an existing tuple.
let john = User(id = 9834, name = "john", displayName = "John", bio = "The one")
let evaContents = { id = 9835, name = "eva", displayName = "Eva!!", bio = "" }
let eva = User(evaContents)
```

### Using multiple constructors

Records can also be used as tagged unions (Algebraic Data Types). This means that the same type can define multiple different "versions", each of which can define its own contents (or no content). Each version is referred to as a constructor, and the constructor used when creating the type defines the final contents.

```yana
-- Here we define a type that represents the suit of a card.
-- Note that Suit is the name of the type,
-- but the constructors all have a different name.
data Suit = Hearts | Spades | Clubs | Diamonds

-- We also define a type for the value of a card.
-- Since the constructor names are quite generic, we make the type qualified.
-- This means that both the type name and constructor name are used to refer to one.
-- Note that the Numbered constructor contains additional data (the actual number).
data qualified Value = Ace | Numbered(Int) | Jack | Queen | King

-- Now we can also define a Card type, which consists of both a suit and a value.
data Card { suit: Suit, value: Value }

-- Finally, let's write a function that gives a numerical value for the worth of a card.
-- Here we use a match expression to check what constructor the Value was created with.
-- The Numbered constructor already contains the value itself,
-- while for the others we return a number that indicates the correct precedence.
fn worth(card: Card) = match card.value:
    Value.Numbered(value) -> value
    Value.Jack  -> 11
    Value.Queen -> 12
    Value.King  -> 13
    Value.Ace   -> 14
```

## Type arguments

There are also cases where it is useful to define a reusable template with a placeholder type.
That allows us to use the same type name in multiple places, but replace the placeholder with a different type each time.
For this reason, alias and record definitions can have type arguments. A type argument is simply a placeholder that is replaced with a real type once the containing type is used.

```yana
-- Here we define an alias with a single type argument `a`.
-- When the alias is used, every instance of `a`
-- is replaced with the type we send in.
-- Note that type arguments always start with a lowercase letter.
alias Vector(a) = { x: a, y: a }

-- We can call the alias when defining another type, just like a function.
-- IntVector replaces `a` with Int, and now is an alias to { x: Int, y: Int }.
-- FloatVector replaces `a` with Float, and now is an alias to { x: Float, y: Float }.
alias IntVector = Vector(Int)
alias FloatVector = Vector(Float)

-- When constructing an alias with type arguments,
-- the compiler will automatically infer what to replace them with based on your input.
-- In this case we send in two Doubles, so the final type becomes { x: Double, y: Double }.
let vector = Vector(3.0, 5.0)

-- Type arguments can also be used with records.
-- Here we define a record with two type arguments
-- that implements a two-way association between types.
data MultiMap(a, b) { aIndex: [a -> b], bIndex: [b -> a] }

-- Finally, type arguments also work when there are multiple constructors in a type.
-- This type defines a simple expression tree for mathematical calculations.
data Expr(n)
    = Literal(n)
    | Add { lhs: Expr(n), rhs: Expr(n) }
    | Mul { lhs: Expr(n), rhs: Expr(n) }

-- Let's implement a function to evaluate an expression containing floats.
-- This function looks at the constructor used, then performs the appropriate calculation.
-- Since some Expr-constructors contain other expressions,
-- `evaluate` may be called recursively.
fn evaluate(expr: Expr(Float)) = match expr:
    Literal(number)  -> number
    Add { lhs, rhs } -> evaluate(lhs) + evaluate(rhs)
    Mul { lhs, rhs } -> evaluate(lhs) * evaluate(rhs)
```