#pragma once

#include "module.h"

/*
 * The semantic index - Implementation-Tooling.md §1.
 *
 * What a name meant, recorded where the answer was produced. Name resolution answers "what does
 * this name refer to" tens of thousands of times per compile and drops every answer once the IR
 * instruction is emitted; this keeps them, keyed by where the name was written.
 *
 * Recorded at the choke points rather than re-derived by a second traversal, and that is the whole
 * design decision. `resolve/name.h`'s search() decides qualified names, import aliases,
 * include/exclude lists, local-shadows-import and ambiguity, and matchInstance decides specificity
 * between overlapping heads. A separate "IDE" walk that looked names up again would be a second
 * name resolver, and two name resolvers drift on exactly the cases a programmer needs the editor
 * for.
 *
 * `Context::index` is null in a batch compile, so the ordinary driver pays one predictable
 * not-taken branch per lookup and nothing else. That is what makes recording at the choke points
 * affordable at all: the cost is opt-in.
 */

/*
 * What a reference points at.
 *
 * A variant rather than a pointer, because the module-level kinds live in different tables and the
 * function-local kinds are not addressable at all. `payload` and `index` hold whichever handle the
 * kind uses - a ModulePtr, a GlobalPtr, a local index, a field index - and every one of those is a
 * region offset or a small integer, so the whole thing stays a value.
 */
struct Symbol {
    // Scoped deliberately: an unscoped `Module` enumerator would shadow the `Module` type inside
    // this struct's own scope, and the definition module is one of its members.
    enum class Kind: U8 {
        // Module-level.
        Function,
        Global,
        Type,
        Alias,
        Constructor,
        Class,
        ClassFun,

        // Function-local. Not addressable by any handle, so `function` is what scopes them.
        Local,
        Arg,
        Capture,

        // Structural.
        Field,
        TypeVar,
        Module,
        Import,
    };

    Kind kind = Kind::Function;

    // Where the symbol is defined. Null for a synthesized symbol that belongs to no module.
    Module* module = nullptr;

    // The enclosing function, for the three function-local kinds.
    ModulePtr<Function> function = nullptr;

    /*
     * The handle, by kind:
     *   Function      ModulePtr<Function>       Global     ModulePtr<Global>
     *   Type/Alias    TypePtr                   Class      GlobalPtr<TypeClass>
     *   Constructor   GlobalPtr<RecordType>, `index` is which constructor
     *   ClassFun      GlobalPtr<TypeClass>,  `index` is which function
     *   Field         TypePtr of the owner,  `index` is which field
     *   TypeVar       GlobalPtr<GenEnv>,     `index` is which variable
     *   Local/Arg     the slot index          Capture   the environment field
     */
    U32 payload = 0;
    U16 index = 0;

    StringId name = 0;

    // Where to jump to. `kNullLocation` for a symbol whose declaration is not in any file the
    // editor can open - everything in Core and Native, which are compiled into the compiler.
    LocationId definition = kNullLocation;
};

/*
 * One resolved occurrence: `source` is where the name was written, and `target` is what it meant.
 *
 * `type` is the type at this occurrence. Inside a generic body it is the *generic* type, because
 * the body's source text exists once and a specialization re-runs no name resolution - §1.3. What
 * differs per specialization is the type, and hover resolves it against one only when asked.
 *
 * `instance` is the answer hover most wants for a class call: which instance served it. Null for
 * everything that is not a call through a class function, and for a call whose types were still
 * variables where it was written.
 */
struct Reference {
    LocationId source = kNullLocation;
    Symbol target;
    TypePtr type = nullptr;
    ModulePtr<ClassInstance> instance = nullptr;
};

struct SemanticIndex {
    Array<Reference> references;
    Array<Symbol> definitions;

    /*
     * Where a location's answer is, in each array.
     *
     * §1.1 proposed a `byModule` map instead. Every question an editor asks arrives as a position,
     * and a position becomes a LocationId through the position index (§2) - so the lookup this has
     * to be fast at is by location, and grouping by module would leave a scan behind it. The module
     * is still reachable, on the Symbol.
     *
     * `kNullLocation` is `maxLimit<U32>`, which is exactly the key a HashMap<U32, _> reserves as
     * empty - so a null location can never be inserted, which is also what recording skips.
     */
    HashMap<LocationId, U32> referenceByLocation;
    HashMap<LocationId, U32> definitionByLocation;

    /*
     * A later answer replaces an earlier one at the same location.
     *
     * That is the rule §1.2 states from the other side - record the *selected* answer, never the
     * candidate set. A call site looks a plain function up before it knows whether the class half
     * of the overload set will serve the call, so the lookup records a candidate and the selection
     * records what the program means.
     */
    void addReference(const Reference& reference);
    void addDefinition(const Symbol& symbol);

    const Reference* findReference(LocationId source) const;
    const Symbol* findDefinition(LocationId source) const;

    /// Every occurrence of one symbol, `definition` included when it is in this program. A linear
    /// scan: find-all-references is a keystroke a user waits for once, not a lookup in a loop.
    void findOccurrences(const Symbol& symbol, Array<const Reference*>& into) const;

    /// Whether two handles name the same thing. Kind plus payload plus index, and the module for
    /// the local kinds, which are only unique within their function.
    static bool same(const Symbol& a, const Symbol& b);
};

/*
 * Recording.
 *
 * Every one of these is a no-op when `context.index` is null or the location is null, so a call
 * site records unconditionally and says what it knows rather than asking first.
 */
void recordReference(Context& context, LocationId source, const Symbol& target, TypePtr type = nullptr,
                     ModulePtr<ClassInstance> instance = nullptr);
void recordDefinition(Context& context, const Symbol& symbol);

/*
 * The symbol builders, one per kind.
 *
 * Each reads the definition's own name and location out of whatever table holds it, so a call site
 * hands over the handle it already has and nothing else.
 */
Symbol functionSymbol(Module& module, ModulePtr<Function> function);
Symbol globalSymbol(Module& module, ModulePtr<Global> global);
// `name` is what the occurrence wrote, kept as the fallback for a type that has no declaration
// of its own to read one off - a scalar, a tuple, a function type.
Symbol typeSymbol(Module& module, TypePtr type, StringId name = 0);
Symbol aliasSymbol(Module& module, const TypeAlias& alias);
Symbol constructorSymbol(Module& module, ConstructorRef reference);
Symbol classSymbol(Module& module, GlobalPtr<TypeClass> typeClass);
Symbol classFunSymbol(Module& module, GlobalPtr<TypeClass> typeClass, U16 index);
Symbol fieldSymbol(Module& module, TypePtr owner, U16 index, StringId name, LocationId definition);
Symbol typeVarSymbol(Module& module, GlobalPtr<GenType> variable);
Symbol moduleSymbol(Module& target);

/// One word for the kind, for hover and for the fixtures. Stable: the `.expect` files hold it.
StringView symbolKindName(Symbol::Kind kind);

/// Where one parameter sits within a printed signature. Byte offsets into what `describeSymbol`
/// wrote, because that is the form the protocol's signature help wants: a client highlights the
/// active parameter by slicing the label rather than by matching text against it.
struct SignatureParameter {
    U32 start = 0;
    U32 end = 0;

    // What a call site names this position by, or zero where it has no name to be named by - a
    // positional field of a constructor. It is what makes signature help follow a named argument to
    // the parameter it fills rather than to the one in its place.
    StringId name = 0;
};

/// The signature line an editor shows for a symbol - `fn map(f: (a) -> b, xs: [a]) -> [b]`, or the
/// binding's own `let &count: Int`. Falls back to the kind and the name for a symbol whose shape
/// there is nothing to print, which is what the three structural kinds are.
///
/// `parameters`, when given, is filled with one entry per written parameter of a function or class
/// function and left alone for every other kind. One printer for both surfaces: a signature help
/// that built its own label would drift from the one hover shows for the same name.
void describeSymbol(Context& context, const Symbol& symbol, TypePtr type, StringBuilder& into,
                    Array<SignatureParameter>* parameters = nullptr);
