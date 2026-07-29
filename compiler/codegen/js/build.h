#pragma once

#include "gen.h"
#include "../../resolve/generic.h"
#include "../../resolve/witness.h"
#include "../../repr/table.h"

/*
 * The JS backend's internal interface - the generator state, the AST construction helpers, and the
 * declarations the emission files share.
 *
 * `gen.h` is what the rest of the compiler sees; this is what the backend sees of itself. The split
 * exists because the target is one pass over the resolve IR that answers six separable questions -
 * what things are called, what shape a type has, what a place is, what an instruction does, what the
 * control flow is, and what gets emitted at all - and each of those is a file here.
 *
 * Everything in this header is inline and small enough to be: allocating a node, naming a statement
 * list, building a call. A helper that has to look at the IR to answer belongs in one of the .cpp
 * files, declared at the bottom.
 */
namespace js {

constexpr U32 kNoBlock = maxLimit<U32>;

/*
 * One block an enclosing construct can be left through, and how.
 *
 * A `Loop` entry is its header, so reaching it is `continue`; a `Forward` entry is a labelled block
 * whose end is that block, so reaching it is `break`. Between them these are every edge JS has no
 * `goto` for.
 */
struct Exit {
    U32 block;
    Name label;
    bool loop;
};

// One cell of a constant table that names a table emitted after it - see genGlobal.
struct Forward {
    Name table;
    U32 cell;
    ModulePtr<Global> target;
};

/*
 * A borrow that had to be boxed at the point it was taken rather than at the storage it names.
 *
 * Only for a borrow of a *field* whose type is not an object - `&p.x` where `x` is an `Int`. The
 * box is a temporary, so the value written through it has to be copied back into the field once the
 * borrow's consumer is done with it, which is Design.md's materialize/write-back around a packed
 * field arrived at from the other direction.
 */
struct Writeback {
    ModulePtr<Value> borrow;
    JsPtr<Expr> box;
    JsPtr<Expr> place;
};

struct Gen {
    Context& context;
    Program& program;
    File& file;
    GlobalBase global;
    ModuleBase local;
    JsBase base;

    /*
     * This target's layout answers, owned by this target.
     *
     * Built from jsReprTarget() in genProgram, and the native backend builds its own from
     * nativeReprTarget(). Neither can see the other's: `Maybe(Id)` is one machine word over there
     * and `number | null` here, and the whole reason Repr is computed at emission rather than during
     * resolution is that both of those are right.
     */
    ReprTable& repr;

    // Identifiers already handed out. Module-level names are checked by every local name as well,
    // so that a local can never shadow a function it might want to call.
    HashSet<StringId> moduleNames;
    HashSet<StringId> localNames;

    HashMap<U32, Name> functionNames;
    HashMap<U32, Name> globalNames;
    HashSet<U32> emittedGlobals;
    Array<Forward> forward;
    Name tableName;

    // Functions this target cannot emit - see excludeFunctions().
    HashSet<U32> excluded;

    // Names this generator has already copied into the string arena - see internText.
    HashSet<StringId> interned;

    // The property names that are the compiler's rather than the program's.
    Name tagField;
    Name payloadField;
    Name boxField;
    Name envField;
    Name headerField;

    // The tuple ClosureHeaderLayout is also described as - see closureHeaderPlaceType. A place into
    // one is a cell read rather than a property, because the header is a compiler-built table here
    // exactly as it is bytes there.
    TypePtr headerType = nullptr;

    /*
     * Per function.
     */

    Function* function = nullptr;
    ModulePtr<Function> functionPointer = nullptr;
    StmtList* body = nullptr;

    HashMap<U32, JsPtr<Expr>> values;
    HashMap<U32, Name> phis;

    // The code word each function-value local has been given, by local index, between the two Inits
    // that build one - see genFunValueWord.
    HashMap<U32, U32> pendingCode;

    // Which locals are stored as a one-property box, by local index. See gen.cpp's file comment.
    Array<bool> boxed;

    // The borrow and address values that are a second *name* for the storage they were taken of
    // rather than a box holding it - see prepareLocals. A place rooted in one of these reaches the
    // storage directly, which is what makes a borrow that never leaves the function cost nothing.
    HashSet<U32> aliasBorrows;

    Array<Writeback> writebacks;

    // The CFG, in the function's own block order.
    Array<ModulePtr<Block>> blocks;
    HashMap<U32, U32> blockIndex;
    Array<Array<bool>> postDominators;
    Array<U32> ipdom;
    Array<U32> idom;
    Array<bool> loopHeader;
    Array<bool> emitted;

    // The constructs currently open that control can leave through - see emitChain.
    Array<Exit> exits;

    U32 labelCounter = 0;

    // The erased half, set only while a generic function is being emitted - the same two fields
    // resolve/lower.cpp carries, for the same reason.
    JsPtr<Expr> genEnv = nullptr;
    GenEnv* genContext = nullptr;
    Module* genModule = nullptr;
};

/*
 * Allocation.
 */

template<class T, class... A>
T* make(Gen& g, A&&... args) {
    return new (g.file.arena) T(forward<A>(args)...);
}

template<class T>
JsPtr<Expr> asExpr(Gen& g, T* value) {
    return (Expr*)value - g.base;
}

template<class T>
JsPtr<Stmt> asStmt(Gen& g, T* value) {
    return (Stmt*)value - g.base;
}

template<class T>
void emit(Gen& g, T* stmt) {
    g.body->push(g.file.arena, asStmt(g, stmt));
}

// Builds one statement list, with everything emitted while `f` runs going into it.
template<class F>
StmtList collect(Gen& g, F&& f) {
    StmtList list;
    auto previous = g.body;
    g.body = &list;
    f();
    g.body = previous;
    return list;
}

// Every instruction of a function, in block order - what the passes that read a whole body before
// anything is emitted (what is expressible, what it reaches, which locals are boxed) walk it with.
template<class F>
void eachInstruction(Gen& g, Function& function, F&& f) {
    for(auto blockPointer: function.blocks.contents(g.local)) {
        for(auto instructionPointer: g.local[blockPointer]->instructions.contents(g.local)) {
            f(*g.local[instructionPointer]);
        }
    }
}

/*
 * Names. The rest of naming is in name.cpp; these two are here because every builder below reaches
 * for them.
 */

// A name this generator spelled out itself rather than one derived from the program - `$tag`,
// `Math`, `BigInt`. No disambiguation, because nothing else may claim these.
Name literalName(Gen& g, StringView text);

// A property name. These are not identifiers in a scope, so they need no disambiguation - two
// records may both have an `x`, and a reserved word is a legal property name.
Name propertyName(Gen& g, StringView text);

/*
 * Expression builders.
 */

inline JsPtr<Expr> variable(Gen& g, Name name) {
    return asExpr(g, make<VarExpr>(g, name));
}

inline JsPtr<Expr> field(Gen& g, JsPtr<Expr> object, Name name) {
    return asExpr(g, make<FieldExpr>(g, object, name));
}

inline JsPtr<Expr> number(Gen& g, F64 value, bool integral = true) {
    return asExpr(g, make<NumberExpr>(g, value, integral));
}

inline JsPtr<Expr> bigInt(Gen& g, U64 value, bool isSigned) {
    return asExpr(g, make<BigIntExpr>(g, value, isSigned));
}

inline JsPtr<Expr> boolean(Gen& g, bool value) {
    return asExpr(g, make<BoolExpr>(g, value));
}

inline JsPtr<Expr> nullValue(Gen& g) {
    return asExpr(g, make<NullExpr>(g));
}

inline JsPtr<Expr> elementAt(Gen& g, JsPtr<Expr> array, JsPtr<Expr> index) {
    return asExpr(g, make<IndexExpr>(g, array, index));
}

inline JsPtr<Expr> index(Gen& g, JsPtr<Expr> array, U32 slot) {
    return elementAt(g, array, number(g, F64(slot)));
}

inline JsPtr<Expr> binary(Gen& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    return asExpr(g, make<BinaryExpr>(g, op, lhs, rhs));
}

inline JsPtr<Expr> unary(Gen& g, UnaryOp op, JsPtr<Expr> value) {
    return asExpr(g, make<UnaryExpr>(g, op, value));
}

inline JsPtr<Expr> ternary(Gen& g, JsPtr<Expr> cond, JsPtr<Expr> then, JsPtr<Expr> otherwise) {
    return asExpr(g, make<TernaryExpr>(g, cond, then, otherwise));
}

inline JsPtr<Expr> assign(Gen& g, JsPtr<Expr> target, JsPtr<Expr> value) {
    return asExpr(g, make<AssignExpr>(g, target, value));
}

// The call an emitter builds when it knows its arguments: `call(g, f, a, b)`. The arity-at-runtime
// form is below, for the instructions that forward an argument list they were handed.
template<class... A>
JsPtr<Expr> call(Gen& g, JsPtr<Expr> callee, A... args) {
    auto node = make<CallExpr>(g, callee);
    (node->args.push(g.file.arena, args), ...);
    return asExpr(g, node);
}

inline JsPtr<Expr> callWith(Gen& g, JsPtr<Expr> callee, Array<JsPtr<Expr>>& args) {
    auto node = make<CallExpr>(g, callee);
    for(auto arg: args) node->args.push(g.file.arena, arg);

    return asExpr(g, node);
}

// A call this generator built out of a host intrinsic rather than out of the program - see
// CallExpr::pure, which is what the two builders below exist to set.
inline JsPtr<Expr> asPureCall(Gen& g, JsPtr<Expr> value) {
    ((CallExpr*)g.base[value])->pure = true;
    return value;
}

// `Namespace.member(...)` for the handful of host intrinsics the integer tower needs - `Math.imul`,
// `BigInt.asIntN`. Nothing else in the emitted code reaches for the host.
template<class... A>
JsPtr<Expr> hostCall(Gen& g, StringView object, StringView member, A... args) {
    return asPureCall(g, call(g, field(g, variable(g, literalName(g, object)),
                                       literalName(g, member)), args...));
}

// `Name(...)` - `BigInt(x)`, `Number(x)`. These are conversions rather than members of anything.
template<class... A>
JsPtr<Expr> globalCall(Gen& g, StringView name, A... args) {
    return asPureCall(g, call(g, variable(g, literalName(g, name)), args...));
}

/*
 * Statement builders.
 */

inline void emitExpr(Gen& g, JsPtr<Expr> value) {
    emit(g, make<ExprStmt>(g, value));
}

// `var name = value;`, and the name is what everything downstream uses for the value.
inline JsPtr<Expr> declare(Gen& g, Name name, JsPtr<Expr> value) {
    emit(g, make<DeclStmt>(g, name, value, false));
    return variable(g, name);
}

/*
 * name.cpp - identifiers.
 */

// Copies text the generator built into the string arena before interning it, since
// Context::addUnqualifiedName keeps the pointer it is given rather than copying.
StringId internText(Gen& g, StringView text);

// A module-level or local identifier, disambiguated against every name already handed out.
Name uniqueName(Gen& g, StringView text, bool local);

// `v7` for a value the source never named, so that the emitted code and the resolve dump agree on
// what to call it.
Name generatedName(Gen& g, StringView prefix, U32 index);

Name valueName(Gen& g, Value& value);
Name fieldName(Gen& g, StringId name, U16 index);

/*
 * type.cpp - what a type is on this target.
 */

IntType* intType(Gen& g, TypePtr type);
RecordType* recordType(Gen& g, TypePtr type);
bool isBool(Gen& g, TypePtr type);
bool isLong(Gen& g, TypePtr type);

// Whether a value of this type is a host object - what `isMemoryType` is on native, asked of this
// target instead. See type.cpp for the three places the two answers differ.
bool isJsObject(Gen& g, TypePtr type);

/*
 * The properties one value of this type has, in construction order.
 *
 * One walk, used by everything that has to agree about the shape of a type: what a fresh slot holds,
 * what a copy duplicates, and what a block copy moves. Two of those disagreeing would be a bug that
 * only shows up as a polymorphic call site or a lost field, so they read the order from here rather
 * than each writing the walk.
 *
 * A sum flattens every constructor's payload into one object - see gen.cpp's file comment - so two
 * constructors that both name a field share the property. That is sound because only one of them is
 * live at a time, and it is what keeps the type to one hidden class instead of one per constructor.
 */
template<class F>
void eachProperty(Gen& g, TypePtr type, F&& f) {
    if(!type || isUnit(g.global, type)) return;

    auto value = g.global[type];

    // A function value has no properties here: it is a host function, and the two words
    // FunValueLayout describes are the closure itself and what it closed over.
    if(value->kind == Type::Fun) return;

    if(value->kind == Type::Tup) {
        U16 slot = 0;
        for(auto entry: ((TupType*)value)->fields.contents(g.global)) {
            f(fieldName(g, entry.name, slot), entry.type);
            slot++;
        }

        return;
    }

    if(value->kind != Type::Record) return;

    auto record = (RecordType*)value;
    if(record->layout == RecordType::Enum) return;

    if(record->layout == RecordType::Single) {
        if(record->constructors.isNotEmpty()) {
            eachProperty(g, record->constructors.get(g.global, 0).content, forward<F>(f));
        }

        return;
    }

    f(g.tagField, g.program.scalar.int_);

    Array<StringId> seen;
    auto payload = false;

    for(auto constructor: record->constructors.contents(g.global)) {
        auto content = constructor.content;
        if(!content || isUnit(g.global, content)) continue;

        if(g.global[content]->kind != Type::Tup) {
            payload = true;
            continue;
        }

        U16 slot = 0;
        for(auto entry: ((TupType*)g.global[content])->fields.contents(g.global)) {
            auto name = fieldName(g, entry.name, slot);
            slot++;

            auto known = false;
            for(auto existing: seen) if(existing == name.text) known = true;
            if(known) continue;

            seen.push(name.text);
            f(name, entry.type);
        }
    }

    // A constructor whose content is not a tuple has no field names to flatten, so its payload is
    // one property of its own.
    if(payload) f(g.payloadField, nullptr);
}

/*
 * Whether this type is a newtype: a single-constructor record over something that is not a tuple,
 * which on this target *is* the value it wraps and has no object of its own - Analysis-JS.md part
 * 1's "newtype: the underlying value, no wrapper". `content` is what it wraps, and is null for a
 * constructor with no content at all, which the callers treat the same way they treat unit.
 */
bool isNewtype(Gen& g, TypePtr type, TypePtr& content);

// The value a freshly allocated slot of this type holds, with every property it will ever have
// already present - see type.cpp.
JsPtr<Expr> zeroValue(Gen& g, TypePtr type);

// A one-property box: what a reference to something that is not an object has to be.
JsPtr<Expr> boxOf(Gen& g, JsPtr<Expr> value);

// An arithmetic result back in its type's range - the integer tower of Analysis-JS.md §2.1.
JsPtr<Expr> coerce(Gen& g, TypePtr type, JsPtr<Expr> value);

// A structural duplicate, property by property - the one ownership operation that costs anything
// here (§2.5).
JsPtr<Expr> cloneValue(Gen& g, TypePtr type, JsPtr<Expr> source, LocationId where);

// The shape a `Native` block copy moves, or null where it is not one whole value of one type.
TypePtr blockCopyShape(Gen& g, InstNative& instruction);

/*
 * place.cpp - values, places and the erased tables.
 */

JsPtr<Expr> constantValue(Gen& g, Value& value);
JsPtr<Expr> useValue(Gen& g, ModulePtr<Value> pointer);

// The emitted name of a module-level global.
JsPtr<Expr> globalValue(Gen& g, ModulePtr<Global> pointer);

JsPtr<Expr> placeExpr(Gen& g, const Place& place);
TypePtr placeType(Gen& g, const Place& place);

// The argument a teardown or a relocation takes: those are written against a raw pointer, so a
// value that is not an object has to arrive in a box the way any other reference does.
JsPtr<Expr> referenceTo(Gen& g, const Place& place);
JsPtr<Expr> referenceTo(Gen& g, TypePtr type, JsPtr<Expr> value);

// A constant table is an array of 32-bit cells, so every offset the native side loads at becomes a
// cell index. `>> 2` is the whole of the translation, and it is exact because every pointer field in
// these layouts is eight-byte aligned and every scalar field is four.
JsPtr<Expr> tableCell(Gen& g, JsPtr<Expr> table, U16 slot);
JsPtr<Expr> genSlot(Gen& g, U16 slot);
JsPtr<Expr> genWitness(Gen& g, U16 slot, ModuleList<U32, false> path);
JsPtr<Expr> genTypeDesc(Gen& g, TypePtr type);

/*
 * inst.cpp - instructions.
 */

void genInstruction(Gen& g, ModulePtr<Inst> pointer);

// The emitted name of a function, with a diagnostic where this target does not have it.
JsPtr<Expr> functionValue(Gen& g, ModulePtr<Function> callee, LocationId where);

/*
 * flow.cpp - control flow.
 */

// The CFG in the function's own block order, plus dominance and the loop headers it decides.
void prepareCfg(Gen& g, Function& function);

// Emits blocks from `block` up to but not including `stopAt`, recovering `if`, `for(;;)` and
// labelled `break`/`continue` from the graph.
void emitChain(Gen& g, U32 block, U32 stopAt);

/*
 * opt.cpp - the peephole between the tree and the text.
 */

// Removes the bindings nothing needed and folds the writes that build a value into the literal that
// builds it. Runs over the finished file, because how many readers a binding has is not known until
// the function it is in has been emitted.
void optimizeFile(Gen& g);

} // namespace js
