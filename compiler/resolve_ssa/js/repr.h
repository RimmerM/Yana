#pragma once

#include "module.h"
#include "../convert.h"

/*
 * What a value *is* on this target, and how an occurrence of one is carried -
 * Analysis-IR-JS.md §3.2's two layers.
 *
 * ```text
 * jsRep(logical type)       -> JsRepId            what the host value is
 * jsManifest(occurrence)    -> JsManifestation    which JsRep, and how it is carried
 * ```
 *
 * The same shape as `lower/repr.h` and `lower/manifest.h`, so that a reader who knows the native
 * half knows this one. And deliberately a different *type* rather than the same one with different
 * numbers: `repr.h` says why - a `Repr` is bytes at an address, and every number in it is a
 * statement about addressable storage, which is a model this target does not have at all. A
 * `NicheKind::Absent` whose offset, size and range all mean nothing is two readings of one struct.
 *
 * ## What is deliberately not here, on the same terms `manifest.h` states them
 *
 *  - **the key.** A manifestation is selected from a whole `mir::Occurrence` - logical type,
 *    contract, interior shape - on every target.
 *  - **what the language decides.** `RecordType::pin`, `RecordType::layout`, the view table, the
 *    containment cut. All on the logical type in `resolve_mir`, so every family reads one statement.
 *  - **the cost model.** What an aggregate costs is a question about the machine, not about a
 *    layout, and it must not be reachable through this table.
 *
 * ## And the three this target answers differently from the plan
 *
 * `HostString` is the whole of a `String` in every contract but unique mutable (§3.2.2): a window
 * manifests as a copy, and the loan checker is what makes that unobservable. A **transparent**
 * record - one stored field and the rest `@host` - is that field, which is what makes `Array(a)` the
 * host array rather than an object wrapping one. And a two-constructor sum with one payload is
 * **`Nullable`** since J74 - `Maybe(T)` is `T | null`, decided on the logical type by `nicheFoldOf`
 * rather than on the name, with `jsAdmitsAbsent` as the one statement of §3.2's nesting rule.
 */
namespace jsir {

/*
 * `JsManifestation` - which representation an occurrence has, and how it is carried.
 *
 * `Manifestation`'s counterpart, and deliberately not `JsRep` with more fields: "what a
 * `Maybe(Person)` is" and "what a `&Maybe(Person)` is carried as" are the two questions that get
 * confused when one table answers both, and the native side already paid for keeping them apart.
 */
struct JsManifestation {
    // What the value is once it has been reached. For a cell this is the *content*, not the cell.
    JsRepId rep;

    /*
     * Whether this occurrence is carried as a `{$v}` cell rather than as the value.
     *
     * True for a unique mutable borrow of anything that is not a host object, and for that alone. A
     * host object *is* its own replaceable storage - writing through a `&Point` writes properties of
     * an object that stays the same object - and a host string is immutable, so `s.push(t)` rebinds
     * and the borrow needs somewhere to rebind to. That is §3.2.2's third row, generalized to
     * everything the same argument applies to.
     */
    bool needsCell = false;

    /*
     * Whether the value's representation is the callee's rather than this site's - a type variable
     * in a body compiled once.
     *
     * Not produced yet: erased generics are a later slice, and what a site holding one does today is
     * report by name. The field exists so that the planner's shape is honest about where the answer
     * goes rather than acquiring a fourth state later.
     */
    bool erased = false;
};

/*
 * The program-wide tables, and the one arena a body is built in - §5.1 and §7.1.
 *
 * Built once by `planProgram` before any body is converted, which is what makes a representation
 * question a dense indexed load rather than a recursive walk from a type. The arena is reset between
 * functions and nothing outside this directory holds a handle into it.
 */
struct JsTarget {
    JsTarget(Context& context, mir::Program& program, ssa::Converted& converted):
        context(context), program(program), converted(converted),
        decl(*program.decl), ssa(*converted.arena) {}

    Context& context;
    mir::Program& program;
    ssa::Converted& converted;

    DeclBase decl;
    SsaBase ssa;

    /*
     * The two packing switches, named after the shipping backend's `ReprTarget` knobs they port.
     *
     * `scalarizeRecords` makes a record all of whose fields are narrow numbers one `number` - no
     * object, no hidden class, a copy that is a read. `packFields` makes the narrow fields of a
     * record that stays an object share `$pN` properties. Both **on** since J85. J83 built them
     * off and J84 added the shift-carrying narrow reference a `&` of a packed field needs: the first
     * record that packs puts a `&flags.a` in front of it, so the two had to land together or not at
     * all, which is the shipping backend's rule for the same pair. They stay switches because a
     * `JsirTester` case builds a target with one of them off.
     */
    bool scalarizeRecords = true;
    bool packFields = true;

    // The packed words - see `JsPacking`. `packingOfRecord` names a scalarized record's own row,
    // keyed on the type alone: `JsPlaceKind::BitRange` is built at a projection that knows the
    // record and not the occurrence, so `jsRep` scalarizes only where the ownership shape changes
    // nothing about the packing (`wholePackingOf`).
    Array<JsPacking> packings;
    HashMap<U32, U32> packingOfRecord;

    // One reusable body arena - §7.1's "not one arena reservation per function". Reset after each
    // function becomes AST.
    JsirArena arena { 4 * 1024 * 1024 };

    /*
     * The stable tables. Interned by structure, so equality is ID equality and neither the
     * conversion nor the verifier performs a recursive type walk.
     */
    Array<JsRep> reps;
    Array<JsShape> shapes;
    Array<JsAbi> abis;

    // `JsRep::firstPart` indexes this: the part representations of every `Parts` rep, back to back.
    Array<JsRepId> partList;

    // The memos. Keyed on the type handle and on the occurrence's three words respectively, which is
    // §3.1's rule about what a manifestation is selected from.
    HashMap<U32, JsRepId> repOfType;

    // And the same question at an occurrence's ownership shape, keyed on the pair - see the second
    // `jsRep` overload. Its own map so that the shapeless path, which is nearly every ask, stays one
    // probe against a 32-bit key.
    HashMap<U64, JsRepId> repOfShaped;

    HashMap<U64, JsShapeId> shapeOfRecord;
    HashMap<U32, JsAbiId> abiOfFunction;

    // The one plan a type descriptor's teardown cell is called across - `jsErasedDropAbi`, interned
    // here so that the site that calls one and the site that fills one compare the same object.
    JsAbiId erasedDropAbi;

    // And the plans the two teardown *bodies* are entered across - `jsTeardownBodyAbi`, keyed on the
    // one position that varies, so that a hundred per-type teardowns share as many plans as they
    // have distinct argument conventions rather than one apiece.
    HashMap<U64, JsAbiId> teardownBodyAbis;

    // The conversion's and the optimizer's per-function side tables - see `JsConvertScratch`. Here
    // rather than on those passes because each is constructed per body and these are sized by the
    // widest body the compilation ever sees.
    JsConvertScratch scratch;
    JsOptScratch optScratch;

    // And the CFG facts every tier below the conversion reads - `JsCfg`, computed once per body by
    // `jsCfgOf` and invalidated by `beginBody`.
    JsCfg cfg;

    // §6.8's block-argument classes and the scratch they are computed in - `opt_coalesce.h`.
    JsCoalesced coalesced;
    JsCoalesceScratch coalesceScratch;

    // Text this table has copied into the string arena - see `jsInternText`.
    HashSet<StringId> interned;

    /*
     * The records that are some lifted lambda's **environment** - the one family this target does
     * not elide.
     *
     * §3.2.1: "a frame-placed closure is `GcObject(shape)` - the environment". That is a statement
     * about *identity* and it is why transparency stops here: an environment holding one captured
     * `Int` would otherwise be that number, and then a closure and the frame that built it would
     * hold two copies of a capture they are supposed to share, a `&` capture would have nowhere to
     * be written, and the pair's second word and the lambda's own parameter would disagree about
     * which of the two they are (a value here, a `{$o, $k}` descriptor there). `ClosureDrop.yana` is
     * where the disagreement arrives, and it arrives as a wrong answer rather than a refusal.
     *
     * Collected by `planProgram` before any representation is interned, because `jsRep` memoizes.
     */
    HashSet<U32> environmentRecords;

    /*
     * The records `transparentContentOf` is asking about right now, innermost last - its cycle
     * guard, for `mir::hidesLoans`' reason. The question recurses into a single field's record and,
     * for a shared loan, through `jsRep` back into itself, and `data Solo {inner: 'Solo}` - a type
     * with no value, but a legal one - asked it without bound: a function taking one crashed the
     * compile with the stack. A record met again while it is being asked is answered "not
     * transparent", so a cycle keeps its object. A non-cyclic type never meets one, which is why
     * the guard changes no answer the corpus sees. Rejected: moving the shared-loan test above the
     * recursion, which was proposed for this and does not end it - that test is a `jsRep`, and
     * `jsRep` memoizes only after it has asked this.
     */
    SmallArray<U32, 8> transparentAsking;

    /*
     * The fields a **property witness** names, keyed `(record type << 32) | field` - and so the
     * fields that never pack (J85).
     *
     * A property witness on this target is a *name*: the body compiled once reads `owner[key]`, so
     * the witnessed field has to be a property of its own. A field packed into a `$pN` word, or a
     * record that is one number, has none. The shipping backend hands the body a `{read, set}` pair
     * of accessors instead; building that here would change the erased body's read from a property
     * access into two calls for every witness, packed or not, to serve a layout this target is free
     * not to choose. So the witnessed field keeps its property and its neighbours still pack.
     *
     * Collected by `planProgram` before any representation is interned, for `environmentRecords`'
     * reason: `jsRep` memoizes, and every environment is interned by the MIR stage before JSIR runs.
     */
    HashSet<U64> witnessedFields;

    /*
     * The globals stored **boxed** - `var counter = {$v: 0}` - because something takes a reference
     * to one.
     *
     * A reference on this target is the `{$o, $k}` descriptor naming a property of something, and a
     * module-level binding is not a property of anything: there is no object to be the `$o`. So a
     * global whose address is handed anywhere gets the one object that can be, on exactly the terms
     * the demand pass boxes an escaping local (`Conversion::escapes`). `Gen::boxedGlobals` is the
     * shipping backend's answer to the same question and the set is computed from the same facts.
     *
     * Whole-program and therefore `planProgram`'s, and it has to be: a global is borrowed in one
     * function and read in another, and the two never see each other, so a shape they disagree about
     * is a value written one way and read the other.
     *
     * A global whose value already *is* a host object is not here, and that is `jsManifest`'s rule
     * rather than an economy - writing through a reference to one writes its properties, so the
     * object is its own reference and a box around it would be the second reference form §3.5 exists
     * not to have.
     */
    HashSet<U32> boxedGlobals;

    /*
     * The representation of a type currently being interned, so that a recursive one closes.
     *
     * §5.1's "recursive schemas are interned by ID before their members are filled": a record's rep
     * is pushed and memoized before its shape's properties are computed, so a field naming the
     * record back finds the ID rather than recursing forever.
     */
    Array<DeclPtr<mir::Type>> interning;

    // The two rows this file recognizes by role rather than by shape. Read once, because they are
    // program facts and asking `roles.record` per type would be a hash lookup per projection.
    DeclPtr<mir::RecordType> stringRecord = nullptr;
    DeclPtr<mir::RecordType> boolRecord = nullptr;

    // Whether anything has been reported. A JSIR compilation that reported once still walks the rest
    // of the program, so that a sweep names every construct rather than the first.
    bool reported = false;

    /*
     * Whether any **lambda environment** in this program owes an authored teardown - §3.9 asked of
     * the one value a collected target could not otherwise answer for.
     *
     * A drop of a function pair is the one release a site cannot resolve: it cannot name the lambda,
     * so it cannot say whether the captures run anything. Natively that is always a call to
     * `funTeardown`, because the block also has to be handed back; here the collector does that, so
     * where no lambda in the program captures anything with an authored `Drop` there is nothing
     * whatever to run and the drop emits nothing at all.
     *
     * A whole-program fact and therefore `planProgram`'s, computed there over every reachable
     * lambda. The alternative - emitting the call unconditionally and letting the helper find an
     * empty header - is a test and an indirect branch per closure death for a question the compiler
     * already knows the answer to.
     */
    bool closureOwesTeardown = false;

    // What the JSIR optimizer did, over the whole program - see `JsOptCounts`, and `opt.h` for the
    // passes. Summed here rather than per body because the number anybody asks for is the program's.
    JsOptCounts optimizer;

    const JsRep& rep(JsRepId id) const { return reps[id.index]; }
    const JsShape& shape(JsShapeId id) const { return shapes[id.index]; }
    const JsAbi& abi(JsAbiId id) const { return abis[id.index]; }

    /*
     * What a declaration is **called** - the one thing the AST tier is allowed to ask `mir::`.
     *
     * §8.1's rule is that nothing below the JSIR-to-AST boundary sees a `mir::Type` at all except
     * to spell a name, and the four sites in `codegen/js/structure.cpp` that reached for `decl[...]`
     * were all doing exactly that - a callee's name for a thunk, a generic's for an environment
     * table, a function's and its arguments' for the emitted parameter list. Written out here so
     * that they name the permission rather than the table: a file that has `decl` can ask it
     * anything, and a file that has these four can ask it what things are called.
     */
    template<class T> StringId nameOf(DeclPtr<T> handle) {
        return handle ? decl[handle]->name : StringId();
    }

    // The declared name of one argument of a function, or nothing where the position has none -
    // which is what the parameter walk over an `JsAbi` meets past the declared arity.
    StringId argumentName(DeclPtr<mir::Function> function, U32 index) {
        if(!function) return StringId();

        auto declared = decl[function];
        if(index >= declared->args.size()) return StringId();

        return decl[declared->args.get(decl, index)]->name;
    }

    // The generic a call's environment table stands for - what that table is named after.
    StringId genericName(DeclPtr<mir::GenEnvironment> environment) {
        if(!environment) return StringId();
        return nameOf(decl[environment]->generic);
    }

    /*
     * One plan into the table, with its derived fields filled - the only way an ABI gets an id.
     *
     * `sealShared` has to run after the last parameter is pushed and before anything reads the
     * plan, and a caller that forgot was a call site the copy elision silently refused. A method
     * rather than a rule, so there is nowhere to forget it: `YanaJsirTester` builds plans by hand
     * and reached exactly that.
     */
    /*
     * The start of a new body - the arena back to empty and the CFG facts thrown away.
     *
     * One call rather than a `target.arena.reset(...)` at each of the four sites that begin one,
     * because the arena is not the only thing that stops describing the last body: `JsCfg` is
     * cached on the first ask and there is nothing in a fresh `JsBody` that says the one before it
     * has gone.
     */
    void beginBody() {
        arena.reset(4 * 1024 * 1024);
        cfg.ready = false;
    }

    JsAbiId addAbi(JsAbi&& abi) {
        abi.sealShared();

        auto id = JsAbiId(abis.size());
        abis.push(::move(abi));
        return id;
    }

    /*
     * What this tier has no answer for yet, by name - §4 and §10.5.
     *
     * Never a `null`, never an empty statement, never a silent skip. The sentence names the
     * construct, because the point of the sweep is an honest worklist: "JSIR does not convert X yet"
     * over a corpus is a list of what to build next, and a fallback value is a fixture that passes
     * for the wrong reason.
     */
    void notYet(StringView what, LocationId where);
    void notYet(StringView what, DeclPtr<mir::Type> type, LocationId where);
    void internalError(StringView what, LocationId where);
};

/*
 * **The** JavaScript representation of one logical type.
 *
 * Memoized on the type handle. By value for `reprOf`'s reason: what it carries is a handful of
 * numbers and two handles, so a copy is words and every caller gets something whose lifetime it does
 * not have to reason about - which matters more here, because `JsTarget::reps` grows while a
 * recursive type is being interned and a reference into it would dangle.
 */
JsRepId jsRep(JsTarget& target, DeclPtr<mir::Type> type, LocationId where);

/*
 * The same, at an occurrence's **ownership shape** - `Maybe('v)` against `Maybe(v)`.
 *
 * A qualified type argument says a field of the record is somebody else's storage, which is a fact
 * about the occurrence rather than about the type: the two are one `mir::RecordType` and two things
 * to hold, one of which has a `{$o, $k}` where the other has the value. §3.1's note that the shape
 * is "usable as part of a manifestation key" is this, and `lower/repr.cpp` keys `membersOf` on the
 * same pair.
 */
JsRepId jsRep(JsTarget& target, DeclPtr<mir::Type> type, DeclPtr<mir::OwnershipSchema> shape,
              LocationId where);

/*
 * The occurrence layer - which representation, and how the value is carried.
 *
 * Takes the whole occurrence, because all three of type, contract and interior shape are inputs and
 * a signature that took fewer would be one a caller can satisfy without having them. That is not
 * hypothetical: `manifestNative` used to take a type and a contract, and every caller passed
 * `ownedContract()` because it had nothing better in hand.
 */
JsManifestation jsManifest(JsTarget& target, const mir::Occurrence& occurrence, LocationId where);

// The shape a `GcObject` names, built and interned on first use.
JsShapeId jsShapeOf(JsTarget& target, DeclPtr<mir::Type> record,
                    DeclPtr<mir::OwnershipSchema> shape, LocationId where);

/*
 * The properties a **transparent** record's value already has - its `@host` fields.
 *
 * `Array(Handle)` is the bare host array, and `length` is a property that array keeps itself. So the
 * record has no shape of its own and its `@host` fields are still reachable: this is the shape a
 * projection into one names, whose keys are the fields' own names because what they are called is
 * the host's answer and not this compiler's.
 */
/*
 * The packing row of a scalarized record, or null where `written.record` is not one - what a
 * projection asks before building a `BitRange` over the record's own place.
 */
const JsPacking* jsWholePackingOf(JsTarget& target, DeclPtr<mir::Type> record);

// The same row as an index into `JsTarget::packings`, or `JsProperty::kNoPacking` - what a `Pack`
// names. The pointer above is for reading and is invalidated by the next row the table grows.
U32 jsWholePackingRow(JsTarget& target, DeclPtr<mir::Type> record);

// A field's value as the `width`-bit pattern it is stored as - the reduction a `Pack` folds with.
// A signed value contributes its two's-complement low bits, exactly as `encodeRange` stores it.
inline U64 jsFieldPattern(F64 value, U32 width) {
    auto bits = U64(I64(value));
    return width >= 64 ? bits : bits & ((U64(1) << width) - 1);
}

JsShapeId jsHostShapeOf(JsTarget& target, DeclPtr<mir::Type> record, LocationId where);

/*
 * Whether a host value of this representation **is** its own storage, and therefore its own
 * reference - the one statement of the rule, read by the two things that box.
 *
 * A host object is: writing through a `&Point` writes properties of an object that stays the same
 * object, so the reference is the object and a descriptor around it would be a second reference
 * form for the one position that does not need one. A host string, a number, a bigint and a host
 * *row* are not - a write through one has to replace a binding somebody else can see, and there is
 * no property for it to land in. `Void` has nothing to name and `Erased` is whatever a caller put
 * there, so neither is boxed: the first has no reference to build and the second would break the
 * agreement `JsPassing::erased` is.
 *
 * `jsManifest`'s `needsCell` is the same question asked of an *occurrence*, and it has to stay the
 * same answer: a position that arrives as a cell and a value that is handed out as one disagreeing
 * is a write into a copy.
 */
bool jsIsItsOwnStorage(JsTarget& target, JsRepId rep);

// The `{$v}` box shape for one content representation - the storage an escaping scalar local gets.
JsShapeId jsBoxShape(JsTarget& target, JsRepId content);

// The `{$o, $k}` descriptor, and the `{$c, $e}` pair - one each per compilation.
JsShapeId jsRefShape(JsTarget& target);
JsShapeId jsFunPairShape(JsTarget& target);

// `Cell(content)` - a reference to storage holding a `content`, spelled `{$o, $k}`. Interned per
// content, so that the verifier can hold a call to the *pointee* representation its plan names.
JsRepId jsCellRep(JsTarget& target, JsRepId content);

/*
 * The same reference **flat** - `(owner, key)`, with `{$o, $k}` as its materialized form.
 *
 * §3.2's "a narrow reference passed flat is `(owner, key, scale)`", with the scale only where a range
 * can exist - `jsRefCarriesScale`, since J84, and on by default since J85. A reference is already a
 * pair of host values, so the object around it
 * buys nothing at a position that takes two arguments - and it costs an allocation at every mutable
 * borrow crossing a call, which is 867 of them in `Base.js` against the shipping backend's 223.
 *
 * Interned per **content**, through the materialized form: the parts themselves are `Erased`,
 * because an owner is any host object and a key is a string or a number, and stating anything
 * narrower would be a lie about a descriptor naming an element. The content is what the verifier
 * still holds a call to, since two contents give two materialized reps and therefore two ids.
 */
JsRepId jsCellPartsRep(JsTarget& target, JsRepId content);

// Whether a reference to `content` crosses as three parts, the third a scale. See repr.cpp.
bool jsRefCarriesScale(JsTarget& target, JsRepId content);

// The `{$v: content}` object itself - what a boxed local and a boxed global are stored in.
JsRepId jsBoxRep(JsTarget& target, JsRepId content);

/*
 * What a value crossing a boundary under this plan actually *is*.
 *
 * `JsPassing::rep` is the content - what the callee sees through the position - and for a `Cell` the
 * host value in the slot is the box around it. Two questions, one field, and the difference is
 * exactly the bug this exists to prevent: a call result declared at the content's representation is
 * a box the caller then reads as if it were the value.
 */
JsRepId carriedRepOf(JsTarget& target, const JsPassing& passing);

// The rep every program has for the two the conversion writes directly.
JsRepId jsVoidRep(JsTarget& target);
JsRepId jsNumberRep(JsTarget& target, NumberDomain domain, U8 bits);
JsRepId jsStringRep(JsTarget& target);
JsRepId jsHostArrayRep(JsTarget& target, JsRepId element, U32 length = 0);

// A host `true`/`false` - the lane of a mask and nothing else. See `JsRepKind::HostBool`.
JsRepId jsHostBoolRep(JsTarget& target);

/*
 * Whether a value of this representation can already be `null`, and therefore cannot be the payload
 * of a niche fold or the thing a `null` literal is built at.
 *
 * §3.2: "nesting is legal only when the inner rep does not already admit the same absent pattern".
 * Read in two places and deliberately one function - `jsRep`, which is what decides a fold, and
 * `verify.cpp`, which is what refuses one that got taken anyway.
 */
bool jsAdmitsAbsent(JsTarget& target, JsRepId id);

/*
 * Whether `null` is a well-formed value of this representation - the verifier's half of the question
 * above, and it differs from it at a host object for the two reasons stated beside it.
 */
bool jsNullIsAValue(JsTarget& target, JsRepId id);

/*
 * A value whose representation the caller decides, and a **table** of such values.
 *
 * The second is what a generic environment, a type descriptor and a class witness all are on this
 * target: a host row whose cells are things this tier deliberately does not describe - the shipping
 * backend's representation table says the same in one line ("`TypeDesc`, `GenEnv`, class witness: an
 * array of cells"). One rep for all three, because what distinguishes them is what a *reader* of a
 * cell does with it and nothing a representation could state.
 */
JsRepId jsErasedRep(JsTarget& target);
JsRepId jsGenTableRep(JsTarget& target);

/*
 * What a `Type` slot points at - the identity of one type, as cells.
 *
 * **Four rather than the native two**, and the difference is the whole of what changes when the
 * target collects its own memory. Natively a descriptor is the five metrics packed into one cell and
 * a teardown in the next, because what a body compiled once needs about a type it cannot see is how
 * many bytes to move and what to run over them. Here there are no bytes to move - a host value is
 * one value whatever it is - so the three measurements have no answer at all and the two facts about
 * *ownership* are the ones a body still asks, beside the teardown that answers them and the zero a
 * fresh slot holds.
 *
 * `Teardown` is null where the type owes nothing, and that is not the native rule: over there a
 * descriptor points at one shared empty body so that a drop is an indirect call rather than a test
 * and a branch. Here the test is `if (d[2])`, the branch is one the engine predicts perfectly, and
 * an empty function per program would be a call the collector makes pointless.
 */
enum JsDescriptorCell: U32 {
    kDescriptorTrivialCopy = 0,
    kDescriptorHasTeardown = 1,
    kDescriptorTeardown    = 2,
    kDescriptorZero        = 3,
    kDescriptorCells       = 4,
};

/*
 * A multi-part representation - §3.2's `Parts { fields, materialized? }`.
 *
 * Interned on the parts and the materialized form together, so equality is id equality and the edge
 * check of §3.8 compares one number. `materialized` may be invalid, which is the "some parts never
 * have a materialized form" row: `requireOne` then reports rather than building one.
 */
JsRepId jsPartsRep(JsTarget& target, Buffer<JsRepId> parts, JsRepId materialized,
                   bool lanewise = false, bool freshEachUse = false);

/*
 * Whether a representation's parts are **lanes** - a vector or a mask - rather than the components
 * of a record. False for everything that is not multi-part at all.
 *
 * The question an operation asks before it distributes over the parts, which is the whole of why
 * `JsRep::lanewise` exists: `convertVectorInst` runs in front of the scalar switch and a `represent`
 * between two window occurrences is not a lane-wise conversion of anything.
 */
bool jsPartsAreLanes(JsTarget& target, JsRepId id);

// The representation of one part, or an invalid id where `parts` is not multi-part or `index` is
// past its end.
JsRepId partRepAt(JsTarget& target, JsRepId parts, U32 index);

/*
 * What **one host value** of this representation is - itself, or a `Parts`' materialized form.
 *
 * The one rule that keeps `Parts` out of every position that holds a single value: a shape property,
 * a place's content, a host array's element, a global, a returned value. A `Parts` representation
 * appears on a `JsPassing`, on a `materialize` and on a `split`, and nowhere else - which is an
 * invariant `verify.cpp` states rather than an economy.
 */
JsRepId jsStoredRep(JsTarget& target, JsRepId id);

/*
 * Whether a record is *transparent* - one stored field, and every field after it `@host`.
 *
 * On this target the wrapper is real: `data Array(a) {items: %a}` is an object with one property
 * costing an allocation and a hidden class per value, and removing it is what makes an array the
 * host array. Natively the same declaration is one word at offset zero of a record that already
 * *is* the word, so there is nothing to remove and the question does not arise - which is why this
 * lives here and not on the logical type.
 *
 * Answers the content type, or null. `@host` fields after the first are the second half of the same
 * statement (Implementation-Containers.md §14): the record is that value and a property it already
 * has, so there is nothing of the record's own left to hold.
 */
DeclPtr<mir::Type> transparentContentOf(JsTarget& target, DeclPtr<mir::Type> record);

// Which typed row a host array of this element is, or `Plain`. A property of the *element*, so one
// rule answers it for a `[U8 *n]`, for `Array(U8)`'s storage and for a constant row alike.
TypedArray typedArrayFor(JsTarget& target, JsRepId element);

/*
 * How many bytes one value of this representation occupies, or zero where the question has no
 * answer on this target.
 *
 * The answer is the **typed row's element width** and nothing else, which is the only byte count
 * this target is entitled to state: a `Uint32Array` element is four bytes by specification, and how
 * many bytes a host object occupies is the engine's answer - it changes with its hidden class, its
 * boxing decisions and its collector, and nothing emitted here can observe it. That is the same
 * sentence `codegen/js/inst.cpp` gives for `sizeOf` and `alignOf` being `@platform(native)`, said
 * as a number where there is one.
 *
 * Zero for everything that has no typed row - a record, a string, a row, an erased type - and the
 * caller reports rather than guessing. The 33-to-53-bit band is the one scalar with no typed row and
 * is eight bytes here anyway, since what a native build would hold it in is a 64-bit word.
 */
U32 jsScalarBytes(JsTarget& target, JsRepId rep);

/*
 * What releasing one value of a type *is* on this target - one step of the teardown walk.
 *
 * There are two walks over this and there is one decision, which is §2.4's rule applied to the one
 * place it was broken here: `jsOwesTeardown` reads the walk for its answer and
 * `convert.cpp`'s `emitTeardown` reads it for its effect, and before this they were two transcripts
 * of one shape that had already drifted apart in two places. The array arm asked different
 * predicates of its element, and only the emitting one had a `Type::Variable` arm - so a sum whose
 * payload is a type variable answered "owes nothing" to the predicate and had a descriptor call
 * written for it by the emitter, and the sum arm is gated on the predicate. That is a release that
 * does not happen.
 *
 * What is deliberately *not* here is how the members are reached: a field is a property of an
 * object and an element is an index, and those are spellings the emitter owns. The step says which
 * arm and which halves, and each caller does its own thing with them.
 */
enum class JsTeardownKind: U8 {
    // Nothing is owed, or the type has no answer on this target and the emitter reports.
    Nothing,
    Unsupported,

    // A function value or an erased iterator - the pair, released through the shared helper.
    FunValue,

    // A `[T *n]`, unrolled over its elements. `content` is the element.
    FixedArray,

    // A value of a type the body was not compiled for - the descriptor's teardown cell.
    Descriptor,

    // A sum, whose derived walk branches on the tag and is therefore a function of its own.
    Sum,

    // A single-constructor record: its fields, or - where it is transparent - its one content.
    Record,
};

struct JsTeardownStep {
    JsTeardownKind kind = JsTeardownKind::Nothing;

    // The authored halves, which run before the derived walk and replace it for their own half.
    bool authoredDrop = false;
    bool authoredReclaim = false;

    // And the halves the members inherit - a `Derived` half the caller still owes.
    bool childDrop = false;
    bool childReclaim = false;

    // `FixedArray`'s element.
    DeclPtr<mir::Type> content;

    bool runsAnything() const {
        return authoredDrop || authoredReclaim || kind != JsTeardownKind::Nothing;
    }
};

JsTeardownStep jsTeardownStep(JsTarget& target, DeclPtr<mir::Type> type, bool runDrop,
                              bool runReclaim);

/*
 * Whether releasing a value of this type runs anything at all on this target.
 *
 * `mir::hasTeardown` answers a question about *storage* and is the wrong one here: a collected
 * target hands nothing back, so a record whose whole teardown is the compiler's walk over members
 * costs exactly the members that owe an **authored** instance and nothing otherwise. This is the
 * step above walked for its answer rather than for its effect.
 *
 * A `Fun` or an `iter(T)` answers `JsTarget::closureOwesTeardown`, because what a pair holds is a
 * run-time fact: the value is whatever lambda reached the site, so the honest answer is the
 * program's. A **type variable** answers yes unconditionally, for the reason the descriptor's
 * teardown cell is never null: what a body compiled once holds is a run-time fact too, and the
 * honest static answer is that something may run.
 */
bool jsOwesTeardown(JsTarget& target, DeclPtr<mir::Type> type, U32 depth = 0);

// How wide this target holds an integer whose width the machine decides - `Size` is a signed 32-bit
// index here and `CodeUnit` is a UTF-16 unit. The same two numbers `targetIntWidths` answers for a
// JavaScript build, stated where the value tower reads them.
U8 jsIntBits(const mir::IntType& type);

} // namespace jsir
