#pragma once

struct Module;

// Defines the temporary pre-typeclass builtin overloads as ordinary resolve-IR
// functions. Their bodies are generated directly, but calls to them are normal calls.
void defineBuiltins(Module& module);
