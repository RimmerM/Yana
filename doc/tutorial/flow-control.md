# Flow control

There are multiple different ways to change the flow of execution in Yana. What way is the best depends on the task at hand, but in most cases it will be similar to other common languages.

## Blocks

In an expression, a colon `:` always indicates the start of a new block.
Blocks are series of statements, each on its own line and starting with the same indentation.
Whenever a new block is opened, the first statement encountered defines the block indentation (as long as it is higher than the previous block).
The first line with a lower indentation than the block closes it.

There are some cases where you can choose yourself if you want to open a new block or not. For example, in functions:
```yana
-- If we start the function with a block by adding ':', 
-- we need to explicitly return a result.
-- Otherwise, the function will simply return nothing.
fn add(a: Int, b: Int):
  return a + b

-- If we use '=' instead, the function becomes a single expression
-- and returns its result automatically.
fn add(a: Int, b: Int) = a + b
```

In conditionals:
```yana
-- Using ':' opens a new block after the condition.
if a > b:
  a = b

-- If we use 'then' instead, the result becomes a single expression.
if a > b then a = b
```

And in pattern matching:
```yana
-- Match handlers that start with `->` are single expressions.
-- Handlers starting with ':' create a new block with multiple lines.
let &x = 1
match { true, 5 }:
    { false, 0 } -> x = 0
    { true, v }  -> x = v
    { false, v }:
        x = 0
        println("{v}")
```

As you can see, the syntax for a new block is always the same, preventing confusion on whether you can use multiple lines somewhere.
The syntax for using a single expression changes for different constructs based on what is more intuitive or clear.

## Declaration guards

As shown before, declarations can contain patterns to destructure rather than defining a single variable.
However, not all patterns are guaranteed to match on their input. For this reason, declarations can contain guards that define what to do if the pattern failed.
This is very useful in some common cases, such as error handling:

```yana
fn printBigNumbers(path: String):
  -- Each of these function calls can fail, 
  -- but by using declaration guards we mostly don't have to think about them. 
  let Ok(file) = File.open(path) | return false
  let Ok(lines) = file.readLines() | return false
  let Ok(values) = lines.map(Int.read).sequence() | return false
  file.close()

  values.forEach: println("{$ * 100}")
  return true

fn parseNumbers(file: File):
  -- Rather than returning from the function, 
  -- a guard can also give an alternative value of the same type.
  -- This allows everything to continue normally.
  let Ok(lines) = file.readLines() | []
  return lines.map: read($) | 0
```

## if/else

If-conditionals are used to change what the program does based on a condition.
They are quite flexible in how they can be used, and can be used as both statements, expressions, and inline similar to the `?:` ternary operator in other languages.

```yana
let n = 5

-- The simplest use - just check a condition and do different things based on the result.
if n == 5:
  println("Everything is as expected")
else:
  println("Something is really wrong")

-- There is no requirement to use an else-clause.
if n == 5: println("Everything is still as expected")

-- Yana does not have "else if". When there are multiple conditions to be checked, 
-- it is much cleaner and more clear to use an if-block.
-- Each line in the block is a condition, 
-- followed by the code to execute if the condition is true.
-- Conditions are evaluated in order of appearance, and `else` always evaluates to true.
if:
  n < 0 -> print("{n} is negative")
  n > 0 -> print("{n} is positive")
  else  -> print("{n} is zero")

-- If-conditionals can also be used an expressions. 
-- This means that the conditional as a whole gives back a result.
-- When the result is used, each path must return the same type.
let bigN = 
  if n < 10 && n > -10:
    println(", and is a small number, increase ten-fold")
    n * 10
  else:
    println(", and is a big number, half the number")
    n / 2

println("{n} -> {bigN}")

-- If you don't need multiple statements inside the block,
-- you can add `then` to keep everything on a single line.
-- This is similar to the x ? y : z ternary operator in other languages.
let sign = if n >= 0 then 1 else -1

```

## while

`while` can be used to execute a block of code until a condition becomes true.

```yana
-- Use `n` to count upwards.
let &n = 1

-- Loop until n is above 100.
while n <= 100:
  if:
    n `rem` 15 == 0 -> println("fizzbuzz")
    n `rem` 3 == 0  -> println("fizz")
    n `rem` 5 == 0  -> println("buzz")
    else            -> println("{n}")
  n = n + 1
```

## for

## match

## if let

## while let
