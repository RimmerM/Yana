# Introduction

Yana is a statically typed language with a syntax inspired by Python and Haskell.
It supports both procedural and functional programming paradigms.

Yana supports compilation to multiple different targets, most notably native and Javascript.
It also has a simple interpreter which can be used to run code in a VM or test without going through the full compilation process.

## Design goals

Yana was designed with the following goals in mind:
 * **Consistent and easy to learn**. The language focuses on providing a limited number of powerful features, which are applied consistently throughout. There should be as few weird edge cases as possible. Most functionality is implemented through simple functions; there is no need for complex class hierarchies from OOP or obscure, impossible to understand type system features from functional programming.
 * **Easy to read**. Code readability is very important for during both implementation, where complicated code makes it difficult to debug, and maintenance, where others need to be able to understand what happens. Yana focuses on readability by making everything that happens explicit - there are no implicit operations or type conversions, and possible mutations are always marked as such.
 * **Almost no runtime errors**. A Yana program has very few places where it could cause runtime exceptions, which makes those places easy to find. Why not "No runtime errors", you might ask? This is mostly because of the two previous design goals. Removing every possible exception source, including operations like fetching from an array, would make the code very convoluted to write or require adding type system features that greatly increase language complexity.
 * **Efficient compilation to Javascript**. The core language should compile to Javascript with no additional overhead for features that are also in Javascript itself. This means that any program written in Yana will be equally fast or faster than the same code in Javascript, and the same size or smaller than the same code in Javascript. Many other languages that compile to JS require downloading a huge runtime even when just writing "Hello World", which makes the resulting code much slower.
 * **Efficient compilation to native**. Yana is a native language, and can be compiled to a native executable. Most Yana programs will use Garbage Collection, since this is required to keep the language and type system simple. The goal here is that native Yana executables should be equivalent in performance to Java, while having a much lower memory footprint. Note however that raw pointers can be used (mostly for interoperation with C), and a Yana program using raw pointers rather than GC will have performance comparable to C.

## Features

Based on the design goals above, a feature set for the language was decided:
 * **Mutability**. Values can be created either mutable or immutable. While immutability has a number of benefits over traditional mutability, there are many cases where it simply makes the code more complex and difficult to understand. For this reason, we support both: the standard library encourages use of immutability, but mutable data is supported where useful.
 * **Pattern matching**. One of the most useful features in many functional languages is pattern matching, both when used for destructuring and for matching on multiple possibilities. Yana extends this with a few simple possibilities, allowing for simple and concise handling of exceptional cases without resorting to either runtime exceptions or complex features like monads.
 * **Side effects**. While representing side effects in the type system has its benefits, it overall makes a language much more cumbersome to use. For this reason, Yana allows functions to have side effects without external tracking.
 * **Simple functional-style type system**. The type system supports standard scalar types, arrays, maps, tuples and records (tagged unions). Together, these can be used to implement everything needed.
 * **Type arguments**. Both functions and record types can have type arguments, which allows implementing generic data types and algorithms that work on many different types. To keep the language simple, type arguments are limited in their functionality; most notably, any functionality used on a generic type is explicitly defined in the function signature.
 * **Typeclasses**. Typeclasses in Yana are used to group up functionality common between types. Similar to the implementation in Rust, they function as both interfaces and a way of function overloading. Typeclasses are especially useful to implement operations that automatically work for many different types.
 * **Actor thread model**. Rather than explicitly creating threads, Yana uses actors that act as lightweight threads. Actors communicate by sending messages to each other. Large objects can be temporarily sent between actors without copying, which gives support for most high-performance uses as well. No shared mutable data is allowed, unless used through raw pointers with manual memory management (native only).
 * **Thread-local garbage collection**. When compiling to the native target, each thread has its own heap. This allows for very efficient garbage collection without ever pausing the whole program; each thread decides for itself when to collect.