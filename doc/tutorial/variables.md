# Variables

A value can be bound to a variable name using the `let` binding.
The compiler automatically infers the variable type based on what is assigned to it.
In cases where you want to be sure that the variable has a certain type, or improve code clarity, an explicit coercion can be added.

```yana
let number = 42
let boolean = false

-- Copy the value of `number` into copiedNumber.
let copiedNumber = number

-- We want this variable to get the type `Long`, so we add a coercion.
let largeNumber = 1 :: Long

println("An integer: {copiedNumber}")
println("A boolean: {boolean}")
```

## Mutability

Variables are immutable by default. This means that what value a variable was bound to cannot change.
Variables can also be made mutable. There are two types of mutability:
 * Reference mutability. References are created using the `&` sign.
   When a variable is a reference, its contents can be changed and the variable itself can be forwarded without being copied.
   In most cases, reference variables will do what you want.
 * Value mutability. Values are created using the `*` sign.
   When a variable is a value, its contents can be changed, but binding the variable elsewhere (such as storing it or using it as a function argument) will copy it. Value variables are only needed in some cases.

```yana
-- This variable cannot be changed after it is created.
let number = 10

-- This variable is mutable with reference semantics.
let &mutableNumber = 11

-- Now, `mutableNumber` will be equal to 22.
mutableNumber = mutableNumber * 2

-- This variable is mutable with value semantics.
-- We only need this in specific situations.
let *value = number
value = 0
```

## Pattern matching

Instead of just binding a value to a name, we can also match the value to specific contents.
This is useful for extracting some part of the value when not everything is needed,
but also to easily perform checks in your code and do something else if they fail.
Yana has extensive support for patterns, and the full possibilities are explained in a separate chapter.

```yana
-- We can use pattern matching to destructure data types and tuples.
let { x, y } = position
let Rect { origin, size } = rectangle

-- We can also use it to check a condition, and break out if it fails.
let Just { 0, y } = position | return Error("position must exist and be on the y-axis")
```

## Scope

Variables only exist in the block they were created in. Each new block is delimited by `:`.
Once the block ends, the variable ceases to exist. Variables can also be shadowed in many cases. This means that, for example, a loop can introduce a variable with the same name as the outer function. Inside the loop, only that variable will be visible. Shadowing can also be used in patterns, to allow destructuring values that will not be used in the rest of the function.

```yana
-- `x` will exist throughout.
let &x = 2
for i in 0..16:
  let power = x * x

  -- However, `power` will not exist beyond the following line.
  x = power
```