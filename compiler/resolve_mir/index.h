#pragma once

#include "decl.h"

namespace mir {

/*
 * The semantic index - Implementation-Tooling.md §1, for this stage.
 *
 * What a name meant, recorded where the answer was produced. Name resolution answers "what does this
 * name refer to" at every occurrence and drops the answer once the node naming it is built; this keeps
 * them, keyed by where the name was written.
 *
 * Recorded at the choke points rather than re-derived by a second traversal, and that is the whole
 * design decision - the same one `compiler/resolve` made before this stage replaced it for editors.
 * `name.h`'s `search` decides qualified names, import aliases, include and exclude lists, local
 * shadowing and ambiguity, and overload selection decides which candidate a call reaches. A separate
 * "IDE" walk that looked names up again would be a second name resolver, and two name resolvers drift
 * on exactly the cases a programmer needs the editor for.
 *
 * `Context::index` is null in a batch compile, so a recording site costs one not-taken branch and
 * nothing else.
 */

/*
 * What a reference points at.
 *
 * A variant rather than a pointer, because the kinds live in different tables and the function-local
 * ones are not declarations at all. Every handle a kind uses is a region offset or a small index, so
 * the whole thing stays a value:
 *
 *   Function, ClassFun  DeclPtr<Function>      Global     DeclPtr<Global>
 *   Type                DeclPtr<Type>          Alias      DeclPtr<Alias>
 *   Constructor         DeclPtr<RecordType>, `index` is which constructor
 *   Class               DeclPtr<Class>         TypeVar    DeclPtr<Type> of the variable
 *   Field               DeclPtr<Type> of the record, `index` is which field
 *   Local, Arg          MirPtr<Local>, or zero for a parameter a completion item names
 *   Capture             the environment field, in the lifted function
 *   Module              nothing - `module` is the answer
 *
 * A class member is `ClassFun` rather than `Function` because the two are different things to an
 * editor - a call through a class is not a direct call - and because a member's own declaration is
 * the one a jump should land on, whichever instance served the call.
 */
struct Symbol {
    enum class Kind: U8 {
        // Module-level.
        Function,
        Global,
        Type,
        Alias,
        Constructor,
        Class,
        ClassFun,

        // Function-local. Not declarations, so `function` is what scopes them.
        Local,
        Arg,
        Capture,

        // Structural.
        Field,
        TypeVar,
        Module,
    };

    Kind kind = Kind::Function;

    // Where the symbol is declared. Null for a type the compiler has of its own - a width, a tuple.
    DeclPtr<mir::Module> module = nullptr;

    // The enclosing function, for the three function-local kinds.
    DeclPtr<Function> function = nullptr;

    U32 payload = 0;
    U16 index = 0;

    StringId name {};

    // Where to jump to. `kNullLocation` for a symbol with no declaration in any file.
    LocationId definition = kNullLocation;
};

/*
 * One resolved occurrence: `source` is where the name was written, and `target` is what it meant.
 *
 * `type` is the type at this occurrence - a binding's own, a function's result, a field's. Inside a
 * generic body it is the *generic* type, because the body's source exists once and a specialization
 * re-runs no name resolution.
 *
 * `instance` is the answer hover most wants for a class call: which instance served it. Null for
 * everything that is not a call of a class member, and for a call whose types were still variables
 * where it was written.
 */
struct Reference {
    LocationId source = kNullLocation;
    Symbol target;
    DeclPtr<Type> type = nullptr;
    DeclPtr<Instance> instance = nullptr;
};

struct SemanticIndex {
    Array<Reference> references;
    Array<Symbol> definitions;

    /*
     * Where a location's answer is, in each array.
     *
     * By location because every question an editor asks arrives as a position, and a position becomes
     * a `LocationId` through the position index. `kNullLocation` is the key a `HashMap<U32, _>`
     * reserves as empty, so a null location can never be inserted - which is also what recording skips.
     */
    HashMap<LocationId, U32> referenceByLocation;
    HashMap<LocationId, U32> definitionByLocation;

    /*
     * A later answer replaces an earlier one at the same location.
     *
     * Record the *selected* answer, never the candidate set: a call site may be looked at twice - an
     * overloaded call is mapped against each candidate before one is chosen - and what the program
     * means is the last word.
     */
    void addReference(const Reference& reference);
    void addDefinition(const Symbol& symbol);

    const Reference* findReference(LocationId source) const;
    const Symbol* findDefinition(LocationId source) const;

    /// Every occurrence of one symbol, its own declaration's included where one was recorded. A
    /// linear scan: find-all-references is a request a user waits for once, not a lookup in a loop.
    void findOccurrences(const Symbol& symbol, Array<const Reference*>& into) const;

    /// Whether two handles name the same thing. The declaration is the identity of the local kinds,
    /// since a slot index is only unique within its function and a name only within its scope.
    static bool same(const Symbol& a, const Symbol& b);
};

/*
 * Recording.
 *
 * Every one of these is a no-op when `context.index` is null or the location is null, so a call site
 * records unconditionally and says what it knows rather than asking first.
 */
void recordReference(Context& context, LocationId source, const Symbol& target,
                     DeclPtr<Type> type = nullptr, DeclPtr<Instance> instance = nullptr);
void recordDefinition(Context& context, const Symbol& symbol);

// Whether anything is recording. For the sites whose symbol is not free to build.
inline bool recording(Context& context) { return context.index != nullptr; }

/*
 * The symbol builders, one per kind.
 *
 * Each reads the declaration's own name and location, so a call site hands over the handle it already
 * has. A `Global` records no module of its own, so its symbol finds the module whose list holds it -
 * a walk, and one only a recording compile takes.
 */
Symbol functionSymbol(DeclBase base, DeclPtr<Function> function);

// `module` where the caller already knows whose list the global is in, which skips the walk.
Symbol globalSymbol(Program& program, DeclPtr<Global> global, DeclPtr<mir::Module> module = nullptr);

// A record answers with its **declaration**: `Maybe(I32)` is made from `Maybe`, and that is where the
// source text is. Everything else is a type with no declaration and names itself.
Symbol typeSymbol(DeclBase base, DeclPtr<Type> type);
Symbol aliasSymbol(DeclBase base, DeclPtr<Alias> alias);
Symbol constructorSymbol(DeclBase base, ConstructorRef constructor);
Symbol classSymbol(DeclBase base, DeclPtr<Class> klass);
Symbol fieldSymbol(DeclBase base, DeclPtr<Type> record, U16 field);
Symbol typeVarSymbol(DeclBase base, DeclPtr<mir::Module> module, DeclPtr<Type> variable);
Symbol moduleSymbol(DeclBase base, DeclPtr<mir::Module> module);

// A name a body bound - a parameter where the local is one, and an ordinary local otherwise.
Symbol localSymbol(DeclBase base, MirBase mir, DeclPtr<Function> function, Local& local);

/*
 * The storage one of the function-local kinds names, or null.
 *
 * By the local's own handle rather than through its function's body, so a binding still answers
 * whether it was declared `&` when the body it is in was dropped for not resolving - the local is in
 * the MIR region either way. Null for a capture, which names a field rather than a local, and for a
 * parameter a completion item names, which is a declaration rather than storage.
 */
Local* symbolLocal(Program& program, const Symbol& symbol);

/// One word for the kind, for hover and for the fixtures. Stable: the `.expect` files hold it.
StringView symbolKindName(Symbol::Kind kind);

/// Whether a name is the compiler's - `$` is not an identifier character, so a name holding one is an
/// entry, a lifted lambda or a thunk. Nothing an editor offers, lists or explains.
bool isCompilerName(Context& context, StringId name);

/// Where one parameter sits within a printed signature, as byte offsets into what `describeSymbol`
/// wrote - which is what signature help wants, since a client highlights the active parameter by
/// slicing the label.
struct SignatureParameter {
    U32 start = 0;
    U32 end = 0;

    // What a call site names this position by, or zero where it has none - a positional payload.
    StringId name {};
};

/*
 * The line an editor shows for a symbol - `fn map(f: (a) -> b, xs: [a]) -> [b]`, or the binding's own
 * `local count: I32`.
 *
 * `parameters`, when given, is filled with one entry per written parameter of a function or a class
 * member, and one per field of a constructor. One printer for both surfaces, so a signature help and
 * a hover cannot describe one name two ways.
 */
void describeSymbol(Context& context, Program& program, const Symbol& symbol, DeclPtr<Type> type,
                    StringBuilder& into, Array<SignatureParameter>* parameters = nullptr);

// A constant the way a declaration wrote it - `Auto`, `-1`, `"text"`. What a default is shown as.
String constantText(Context& context, DeclBase base, const Constant& constant);

} // namespace mir
