# Hello World

Let's start by showing the traditional Hello World program.

```yana
-- This is a comment. It will be ignored by the compiler.
-- We don't need to have a main function. Yana will simply execute any code in each module.
-- Print the text to the console.
println("Hello World!")
```

## Comments

Yana supports two different comment types:
 * `-- Line comments which go to the end of the line.`
 * `{- Block comments which continue until they are closed. -}`

```yana
-- This is an example of a line comment.
-- Each line comment starts with two dashes.
-- Everything until the next line is ignored.

{-
 This is an example of a block comment. The block starts with {-,
 and everything until the closing -} is ignored.
 Block {- comments can also be nested -}, which is useful when commenting out code.
 Note that the preferred style is to use single-line comments.
 -}

-- Writing a comment just before some declaration or definition
-- allows it to be used in generated documentation.
-- Starting the line with an argument or field name followed by ':'
-- binds the comment to that name.

-- Adds two integers.
-- lhs: The first value to add.
-- rhs: The second value to add.
-- return: The sum of the two numbers.
fn add(lhs: Int, rhs: Int) = lhs + rhs
```