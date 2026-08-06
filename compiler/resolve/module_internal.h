#pragma once

/*
 * The declaration pass, shared between the files it is split across.
 *
 * `module.h` is the interface - the four entry points the compiler driver calls are declared there.
 * What is here is the seam between these seven translation units, which are one pass cut by what
 * each of them declares rather than by how long it got:
 *
 *  - module.cpp         - the containers and the order. Program, Module, Function and Block, and
 *                         the sweeps over a module's declarations that decide what is declared
 *                         when. Nothing here reads a declaration's contents; it says which pass
 *                         does.
 *  - module_decl.cpp    - types and globals. Records, newtypes, aliases, constructors, field
 *                         defaults, layout and inline attributes, and a global's declaration.
 *  - module_sig.cpp     - signatures. A function's arguments, its return type, and the return-root
 *                         rules that relate the two.
 *  - module_class.cpp   - classes and instances. A class's declaration, the signatures it holds,
 *                         instance selection, and the superclass obligations an instance takes on.
 *  - module_default.cpp - class default bodies, and the order they may be written in.
 *  - module_import.cpp  - the import graph.
 *  - module_reach.cpp   - reachability, which is the only one of these that runs over a finished
 *                         program rather than over syntax.
 *
 * Only what has a caller in another one of those files is declared here. The declaration order the
 * sweeps in module.cpp impose is the reason so much of it is: the passes are separate, and which
 * one runs first is the one thing they are not free to decide for themselves.
 */

#include "module.h"

// -- module.cpp --------------------------------------------------------------------------------

// One declaration of a module's list, as a pointer. The list is a parse-tree list, so this is the
// index-to-handle step every sweep over it begins with.
ast::ParsePtr<ast::Decl> declAt(ast::DeclList decls, Size index);

// -- module_decl.cpp ---------------------------------------------------------------------------

GlobalPtr<GenEnv> prepareGenEnv(Module& module, GenEnv::Kind kind,
                                ast::ParseList<StringId> variables, ast::ConstraintList constraints,
                                bool open = false);
void resolveConstraintClasses(Module& module, GenEnv& env);
void declareRecordDefaults(Module& module, ast::Decl& decl);
void declareRecord(Module& module, ast::Decl& decl);
void declareNewtype(Module& module, ast::Decl& decl);
void defineRecord(Module& module, ast::Decl& decl);
void defineNewtype(Module& module, ast::Decl& decl);
RecordType* declaredRecord(Module& module, StringId name);
void declareAlias(Module& module, ast::Decl& decl, ast::ParsePtr<ast::Decl> pointer);
void declareGlobal(Module& module, ast::Decl& decl);
void readInlineAttribute(Module& module, const ast::Decl& decl, Function& function);

// -- module_sig.cpp ----------------------------------------------------------------------------

Function* resolveSignature(Module& module, ast::Decl& decl, GenEnv* env, StringId name,
                           bool anonymous, bool classSignature = false);

// -- module_class.cpp --------------------------------------------------------------------------

void declareClass(Module& module, ast::Decl& decl, ast::ParsePtr<ast::Decl> pointer);
void resolveClassSignatures(Module& module, TypeClass& typeClass);
void resolveInstance(Module& module, ast::Decl& decl);
void resolveDefault(Module& module, ast::Decl& decl);
void checkSuperclasses(Module& module, ClassInstance& instance);

// -- module_default.cpp ------------------------------------------------------------------------

ModulePtr<Function> resolveClassDefault(Module& module, TypeClass& typeClass, ast::Decl& member,
                                        ast::ParsePtr<ast::Decl> pointer, Function& signature);
void checkDefaultRanks(Module& module, TypeClass& typeClass);
