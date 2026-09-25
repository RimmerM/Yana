#pragma once

#include "opt.h"
#include "cfg.h"

/*
 * The JSIR optimizer's one `Optimizer`, shared by the files that hold its passes.
 */
namespace jsir {


// `JsUseRole` and `JsUse` are `module.h`'s, because the scratch that holds a run of them is.
using Use = JsUse;

// Whether a call's argument at `ordinal` is a position the callee may only read - one bit of
// `JsAbi::sharedMask`, which is that walk done once where the plan was frozen rather than once per
// argument of every call this pass looks at.
inline bool argumentIsShared(JsTarget& target, JsAbiId abiId, U32 ordinal) {
    if(!abiId.valid() || abiId.index >= target.abis.size()) return false;
    return target.abi(abiId).sharedAt(ordinal);
}

struct Optimizer {
    Optimizer(JsTarget& target, JsBody& body):
        target(target), body(body), base(*target.arena), builder(target, body),
        s(target.optScratch) {}

    JsTarget& target;
    JsBody& body;
    JsirBase base;
    JsBuilder builder;

    /*
     * Every list this pass keeps, on the target rather than here - `JsOptScratch`.
     *
     * The pass is constructed per body, so an `Array` declared as a member of it is an allocation
     * per body per list. The names below are references so that the code reads as it did.
     */
    JsOptScratch& s;

    // One entry per value, into `useList` - the CSR index §7.4 asks for instead of a list per value.
    Array<U32>& useStart = s.useStart;
    Array<Use>& useList = s.useList;

    /*
     * How many live `Load`s and `Stores` name each place.
     *
     * The bookkeeping half of this pass, and the one thing about `JsValue::uses` that is not one use
     * per instruction: `convert.cpp` caches a place per SSA value, so several instructions may read
     * one `JsPlace` while `addProperty` counted its owner **once**. Decrementing per instruction
     * would take an object's count to zero while a live load still named it, and the dead-code walk
     * would then remove the object literal a later statement reads - which is exactly the "met a
     * value with no expression" this pass produced before the count was kept this way.
     */
    Array<U32>& placeRefs = s.placeRefs;

    /*
     * How many uses of each value the *index* sees - one per live instruction that names it,
     * including as a place's owner.
     *
     * Deliberately not `JsValue::uses`, which this pass also maintains and which answers a different
     * question. That one is the construction count `codegen/js/opt.cpp` reads to decide whether an
     * expression may be written at its single use, and it is an over-count by design: a place built
     * by `placeOf` and never read by any instruction still counted its owner, and a `Value` place's
     * load counted the object twice. An over-count is harmless there and fatal here - an object
     * literal whose three property reads have all been forwarded would never reach zero, so the
     * literal would survive with nothing reading it. `Failure {site, claim, detail}` is where that
     * showed.
     */
    Array<U32>& liveUses = s.liveUses;

    // Which body parameters are at a `shared` position, by value id. A parameter this is true of is
    // storage the caller's loan says nothing writes for the length of the call.
    IndexSet& sharedParameter = s.sharedParameter;

    /*
     * Every use one instruction makes, with what that position does to the value.
     *
     * Places are walked here rather than through `forEachUse`, because a place's owner is a use the
     * instruction's own operand list does not carry - `JsInstLoad` holds a `JsPlaceId` and nothing
     * else - and because what the owner of a place *is* depends on whether the instruction reads it
     * or writes it.
     */
    template<class F> void eachUse(JsInst& inst, F&& f) {
        switch(inst.kind) {
            case JsInst::Const:
            case JsInst::FunCode:
            case JsInst::GenTable:
            case JsInst::Unreachable:
                return;

            case JsInst::Load: {
                auto& place = builder.place(((JsInstLoad&)inst).place);
                if(place.owner) f(base[place.owner], JsUseRole::Read);
                if(place.index) f(base[place.index], JsUseRole::Read);
                if(place.bitScale) f(base[place.bitScale], JsUseRole::Read);
                return;
            }

            case JsInst::Store: {
                auto& written = (JsInstStore&)inst;
                auto& place = builder.place(written.place);
                if(place.owner) f(base[place.owner], JsUseRole::Written);
                if(place.index) f(base[place.index], JsUseRole::Read);
                if(place.bitScale) f(base[place.bitScale], JsUseRole::Read);
                if(written.value) f(base[written.value], JsUseRole::Kept);
                return;
            }

            // The pair of nodes whose result *is* their operand's object.
            case JsInst::Alias:
                f(base[((JsInstAlias&)inst).from], JsUseRole::Aliased);
                return;

            // A duplicate reads its source and hands back something else, so the source is only
            // read - which is what makes a clone of a clone collapse.
            case JsInst::Clone:
                f(base[((JsInstClone&)inst).from], JsUseRole::Read);
                return;

            case JsInst::Split:
                f(base[((JsInstSplit&)inst).from], JsUseRole::Read);
                return;

            case JsInst::Object:
            case JsInst::Array:
            case JsInst::Materialize: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Kept);
                });
                return;
            }

            case JsInst::Call: {
                auto& written = (JsInstCall&)inst;
                if(written.genEnv) f(base[written.genEnv], JsUseRole::Kept);

                auto arguments = written.arguments.contents(base);
                for(U32 i = 0; i < arguments.size(); i++) {
                    if(!arguments[i]) continue;
                    f(base[arguments[i]], argumentIsShared(target, written.abi, i)
                        ? JsUseRole::Read : JsUseRole::Written);
                }
                return;
            }

            case JsInst::CallValue: {
                auto& written = (JsInstCallValue&)inst;
                if(written.code) f(base[written.code], JsUseRole::Kept);
                if(written.environment) f(base[written.environment], JsUseRole::Kept);

                auto arguments = written.arguments.contents(base);
                for(U32 i = 0; i < arguments.size(); i++) {
                    if(!arguments[i]) continue;
                    f(base[arguments[i]], argumentIsShared(target, written.abi, i)
                        ? JsUseRole::Read : JsUseRole::Written);
                }
                return;
            }

            /*
             * A host row is one `resolve_mir/host.def` row and this tier does not know what it does
             * with a reference - `hostCall1` may keep it, and a helper's roster includes one that
             * runs an authored teardown. Both refuse rather than being grouped with the calls, whose
             * positions have a declared convention to read.
             */
            case JsInst::Host:
            case JsInst::Helper:
            case JsInst::TypeDrop: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Opaque);
                });
                return;
            }

            // Arithmetic over a value reads it; a `select` or a `map` hands one of its operands
            // back, which is the same thing an edge does with it.
            // A `Pack`'s operands are numbers, read into a new one.
            case JsInst::Pack:
            case JsInst::Binary:
            case JsInst::Unary:
            case JsInst::Coerce:
            case JsInst::Convert:
            case JsInst::Math: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Read);
                });
                return;
            }

            case JsInst::Select:
            case JsInst::Map: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Kept);
                });
                return;
            }

            case JsInst::Throw:
            case JsInst::Return: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Kept);
                });
                return;
            }

            case JsInst::Jump:
            case JsInst::Branch:
            case JsInst::Switch: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Read);
                });

                forEachEdge(base, inst, [&](JsEdge& edge) {
                    for(auto supplied: edge.arguments.contents(base)) {
                        if(supplied) f(base[supplied], JsUseRole::Kept);
                    }
                });
                return;
            }

            case JsInst::KindCount:
                return;
        }
    }

    template<class F> void eachInst(F&& f) {
        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, JsBlockId(i));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(base)) f(*base[pointer]);
        }
    }

    /*
     * Every use of every name `value` goes by, where a name is `value` and whatever an `Alias`
     * of a name produces. `visit(name, use, follow)` answers whether the walk goes on, and may
     * `follow` a value it decides is one more name - a whole read of a `Value` place is the
     * object itself. The walk stops at the first refusal and answers false.
     *
     * One walk for `namesOnlyRead`, `namesOnlyFields` and the rewrite `splitRecordVariables`
     * makes after the second has answered: three copies differing only in what they accepted
     * were three places to forget an `Alias` whose result was never used.
     */
    template<class F> bool forEachName(JsValue* value, F&& visit) {
        auto& names = s.names;
        names.clear();
        names.push(value);

        auto follow = [&](JsValue* name) { names.push(name); };
        while(names.size()) {
            auto name = names.pop().unwrap();
            for(auto& use: usesOf(name)) {
                if(use.role == JsUseRole::Aliased && use.inst->kind == JsInst::Alias && use.inst->result) {
                    names.push(base[use.inst->result]);
                    continue;
                }

                if(!visit(name, use, follow)) return false;
            }
        }

        return true;
    }

    // What is left of `readOnly`'s walk budget for the question being asked; zero refuses.
    U32 budget = 0;

    static constexpr U32 kNoVariable = maxLimit<U32>;

    /*
     * Reverse postorder, immediate dominators and which natural loops hold each block - all of it
     * `jsir::JsCfg`'s since 2026-09-22 and none of it computed here.
     *
     * This file had its own copy, the verifier had another and `codegen/js/structure.cpp` had a
     * third in a different algorithm. They are one graph: nothing in this pass touches an edge, so
     * the CFG a body is converted with is the CFG it is structured with. `jsCfgOf` computes it on
     * the first ask and `JsTarget::beginBody` throws it away.
     *
     * `JsCfg::reducible` is what `sinkable` was: an **irreducible** graph is the one shape the loop
     * masks cannot describe, since a retreating edge whose target does not dominate its source
     * names a cycle with two entries and a block's "which loops am I in" is then not a set of
     * natural loops. The structurer makes the same distinction for its own reasons and finds no
     * such body in `test/base`; here it is a refusal rather than a fallback.
     */
    static constexpr U32 kNoBlock = JsCfg::kNoBlock;

    const JsCfg* flow = nullptr;

    /*
     * One instruction becomes an `Alias` of a value already in scope, keeping its result. The only
     * shape any rewrite here takes, which is what keeps every user of the result untouched.
     *
     * Collected first and applied second - see `pending`. Applying one rewrite while the use index
     * is being read would leave the index describing a body that no longer exists, and the entries
     * it would be missing are exactly the `Aliased` ones this creates.
     *
     * `JsOptRewrite` is `module.h`'s, beside the scratch that holds the list of them.
     */
    using Rewrite = JsOptRewrite;

    Array<Rewrite>& pending = s.pending;

    // The newest entry keyed on `key` that `accept` takes, skipping reads below `barrier`.
    template<class F> JsInst* findNumbered(JsValue* key, bool read, U32 barrier, F&& accept) {
        auto at = s.numberedHead[canonicalOf(key)];
        while(at != maxLimit<U32>) {
            auto& entry = s.numbered[at];
            if(entry.read == read && (!read || at >= barrier) && accept(*entry.inst)) return entry.inst;
            at = entry.previous;
        }

        return nullptr;
    }

    /*
     * An instruction made here rather than through `JsBuilder::add`, which appends to the block it
     * is building. The region list has no insert, so `insertAt` pushes the last entry again and
     * shifts the tail right by one - a block is tens of instructions, and this runs a handful of
     * times per body.
     */
    template<class T, class... A> T* makeInst(JsBlock* block, JsRepId result, A&&... args) {
        auto inst = new (target.arena) T(forward<A>(args)...);
        inst->id = JsInstId(body.insts.size());
        body.insts.push(target.arena, (JsInst*)inst - base);
        inst->block = block - base;

        if(jsInstResult(inst->kind) != kNoResult && result.valid()) {
            auto value = builder.addValue(result);
            value->definer = (JsInst*)inst - base;
            inst->result = value - base;
        }

        return inst;
    }

    /*
     * Whether every name for `value` - it, and every alias of it - is only the owner of a property
     * read, a property write or a bit range over one property, and never the value written. Each
     * place's shape is recorded against `slot` through `agree`, so a variable read through two
     * shapes refuses. Every access must also pass `within`, which is where the caller says *when*
     * the access may happen.
     */
    template<class F, class W> bool namesOnlyFields(JsValue* value, F&& agree, W&& within) {
        return forEachName(value, [&](JsValue* name, Use& use, auto&&) {
            auto& inst = *use.inst;
            if(use.role == JsUseRole::Aliased) return false;

            if(inst.kind == JsInst::Store) {
                auto& store = (JsInstStore&)inst;
                if(store.value && base[store.value] == name) return false;
            } else if(inst.kind != JsInst::Load) {
                return false;
            }

            if(!within(inst)) return false;

            auto& place = builder.place(placeOfAccess(inst));
            if(!place.owner || base[place.owner] != name || place.index || place.bitScale) return false;

            if(place.kind == JsPlaceKind::Property) return agree(place.shape);

            if(place.kind == JsPlaceKind::BitRange && place.word.valid()) {
                auto& word = builder.place(place.word);
                if(word.kind != JsPlaceKind::Property || !word.owner || base[word.owner] != name) return false;
                return !word.index && !word.bitScale && agree(word.shape);
            }

            return false;
        });
    }

    // opt_analysis.cpp: the indexes the passes read - uses, place references, dominance, positions and
    // every touch of a variable - and the edits that keep them true.
    void buildUses();
    void release(JsInst& inst);
    void countPlaceRefs();
    Buffer<Use> usesOf(JsValue* value);
    bool computeDominance();
    U32 commonDominator(U32 a, U32 b);
    void applyRewrites();
    void indexPositions();
    U32 positionOf(JsInst& inst);
    bool strictlyDominates(U32 above, U32 below);
    bool runsBefore(JsInst& inst, JsInst& clone);
    bool runsBeforeInIteration(JsInst& inst, JsInst& clone);
    void indexTouches();
    U32 slotOfPlace(JsPlaceId id, bool& whole);
    JsPlaceId placeOfAccess(JsInst& inst);
    bool isWholeVariable(JsPlaceId id, U32 slot);
    void insertAt(JsBlock* block, Size at, JsInst* inst);
    Size indexIn(JsBlock* block, JsInst* inst);
    void replaceInst(JsInst* old, JsInst* inst);
    void removeInst(JsInst* inst);

    // opt_alias.cpp: whether a copy may be an alias of what it copies, and the literal forwarding that
    // gives it more copies to ask about.
    bool readOnly(JsValue* value, U32 allowStoreInto, U32 depth = 0);
    bool variableIsReadOnly(U32 slot, U32 depth);
    bool isFreshHere(JsValue* value);
    JsValue* rootOf(JsValue* value);
    bool sourceIsStable(JsValue* value);
    void aliasCopies(bool count);
    JsValue* wholeValueRead(JsInst& inst);
    bool namesOnlyRead(JsValue* value);
    JsInst* literalOf(JsValue* value, bool& copied);
    JsirList<JsirPtr<JsValue>, false>* shapedValues(JsInst& inst, JsShapeId& shape);
    bool fieldsMatch(JsirList<JsirPtr<JsValue>, false>& list, JsShapeId shape);
    void forwardFields();
    void dropDead();

    // opt_move.cpp: whether a copy is the last use of what it copies.
    bool refilledFirst(JsInst& load, U32 first, U32 last, Array<JsInst*>& touches);
    bool ownedOutright(JsValue* value, U32 depth = 0);
    void moveLastUses();
    void readThroughCopies();

    // opt_number.cpp: whether a computation, a read or a branch is one a dominating one already answered.
    U32 canonicalOf(JsValue* value);
    bool writesLocal(JsPlaceId id);
    bool clobbersHeap(JsInst& inst);
    bool samePlace(JsPlaceId leftId, JsPlaceId rightId);
    bool readable(JsPlaceId id);
    bool sameComputation(JsInst& a, JsInst& b, bool constants);
    JsValue* keyOperand(JsInst& inst);
    JsValue* keyOwner(JsPlaceId id);
    bool isComparison(JsInst& inst);
    const JsConstant* constantOf(JsValue* value);
    bool isNumberZero(JsValue* value);
    JsValue* identityOf(JsInst& inst);
    void numberConstant(JsInst& inst);
    U32 comparedOf(JsValue* value);
    bool overConstants(JsInst& inst);
    void pushNumbered(JsInst& inst, JsValue* key, bool read);
    void popNumbered(Size to);
    bool readRangeOfValue(JsInst& inst, JsValue* word);
    void numberBlock(U32 index, U32& barrier);
    void enterArm(U32 parent, U32 child);
    void numberValues();
    void decideBranches();
    void unlinkEdge(JsBlock* from, JsBlock* to, Array<U32>& orphans);
    bool constantNumber(JsValue* value, F64& out, U32 depth = 0);
    void foldConstantBranches();
    void foldConstants();
    bool foldedValueOf(JsInst& inst, JsConstant& out);
    const JsConstant* literalOf(JsValue* value);

    // opt_objects.cpp: where an object is built, and whether it needs to be one.
    void adoptInitialStores();
    bool adoptAt(JsBlock& block, Size at);
    void sinkAllocations();
    void dropWriteOnlyObjects();
    void unboxLocals();
    bool straightSuccessor(JsBlock*& block, Size& at);
    bool copyField(JsPlaceId id, IndexSet& owners, U32& slot, bool& bits);
    bool updateAsLiteral(JsInstClone& clone);
    void updatesAsLiterals();

    // opt_variables.cpp: a record local as one variable per field, and what is stored into a variable
    // that nothing reads.
    void retarget(JsInst& inst, JsPlaceId to);
    U32 firstWholeStore(JsBlock* block, U32 slot, U32 from);
    void floodClean(JsBlock* from, U32 slot, bool leavesClean);
    void splitRecordVariables();
    void storeLiteralFields();
    void forwardVariables();
    void dropDeadStores();
    void dropDeadLoads();

    // opt_driver.cpp: the order the passes run in.
    void run();
};

} // namespace jsir
