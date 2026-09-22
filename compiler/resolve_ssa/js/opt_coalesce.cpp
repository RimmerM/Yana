#include "opt_coalesce.h"

/*
 * Analysis-IR-JS.md §6.8 - which block arguments may share one variable.
 *
 * Here rather than in `codegen/js/structure.cpp`, where it was written at J76, and the split is the
 * point: what this answers is a **liveness question about the JSIR CFG**, and what the emitter does
 * with the answer is name a `var`. That file's own README argued the whole thing belonged up there
 * because "the decision about which variable two of them share belongs where the variables are
 * named" - which is true of the naming and was not true of the two hundred lines of dataflow in
 * front of it, in a file whose rule is that it switches on nothing but a `JsRep`, a `JsShape` and a
 * `JsPlace`. So the classes are computed here and the names are still minted there.
 */

namespace jsir {

namespace {

struct Coalescer {
    Coalescer(JsTarget& target, JsBody& body):
        target(target), body(body), base(*target.arena), scratch(target.coalesceScratch) {}

    JsTarget& target;
    JsBody& body;
    JsirBase base;
    JsCoalesceScratch& scratch;

    JsBlock* blockAt(U32 index) { return body.block(base, JsBlockId(index)); }

    /*
     * §6.8, and it is the largest thing left in the emitted text that is not a copy the inliner
     * made.
     *
     * The dominator-tree structuring gives **every join its own argument**, so a value that reaches
     * a `return` through three nested joins is written out as three variables and two copies:
     *
     *     B3: { if (c) { p6 = -2; break B3 } ... if (d) { p7 = x } else { p7 = -2 } p6 = p7 }
     *     p5 = p6;
     *
     * The shipping backend has none of that, because its structurer assigns into the variable the
     * source named: `Base.js` carried 3,327 `var p;` declarations against its 274 and 1,079 copies
     * of the form `x = y;` against its 177. Naming two arguments the same variable makes the copy a
     * self-assignment, which `emitEdge` then writes nothing for.
     *
     * **The condition is non-interference and nothing else**, which is the classical one and is what
     * makes this a renaming rather than a rewrite: if no program point has both values live, one
     * variable holds whichever of them is live there and every read sees what it read before.
     *
     * Two things have to be in the interference relation and only the first is ordinary liveness:
     *
     *  - **a value live anywhere in a block interferes with that block's own arguments.** Liveness
     *    is the standard backward fixpoint, with the one care a block-argument IR needs: an argument
     *    of `S` is *not* live before `S`, because the edge is where it is written. So what a block
     *    hands its predecessors (`up`) excludes its own arguments, and what makes it interfere is a
     *    separate set (`liveAt`);
     *  - **the copies on one edge are sequential, so a destination interferes with every *other*
     *    source on the same edge.** `emitEdge` writes them in order, so without this a destination
     *    coalesced with an unrelated source would be overwritten before the later copy read it -
     *    the parallel-copy hazard, which is the one thing about this rewrite that is not liveness.
     *    Both rules are **measured by weakening them** rather than by a fixture, since no `.yana`
     *    program varies one without varying everything else: with the sequential rule out,
     *    `test/base` fails under Node in `Table_addColumn` with "cannot read properties of
     *    undefined", and with the interference test out it does not terminate at all.
     *
     * Conservative in two ways that only ever refuse: a value is treated as live for the whole of
     * any block it is live *anywhere* in, and a body with more arguments than the matrix below
     * admits is left alone.
     */
    static constexpr U32 kNoArgument = maxLimit<U32>;
    static constexpr U64 kArgumentLimit = 4000000;

    // The dense numbering of the block arguments and the union-find over it, on the target's
    // scratch - see `JsCoalesceScratch`, where the reason none of these is declared here is that
    // this struct is constructed per body.
    Array<U32>& dense = scratch.dense;
    Array<U32>& valueId = scratch.valueId;
    Array<U32>& parent = scratch.union_;
    U32 argTotal = 0;

    U32 classOf(U32 dense) {
        while(parent[dense] != dense) dense = parent[dense];
        return dense;
    }

    /*
     * Whether any member of one class interferes with any member of another, kept as a **row per
     * class** rather than asked of every pair.
     *
     * The pairwise form is what this had first and it is cubic - the scan is over every argument
     * twice, once per merge - which took one `Base.js` build past two minutes. The row is the union
     * of its members' rows and is merged with them, so a query is the size of the smaller class.
     *
     * The rows live on the target's scratch - see `JsCoalesceScratch`.
     */
    bool classesInterfere(U32 x, U32 y) {
        auto row = scratch.interferes.row(x);
        for(auto member: scratch.classMembers[y]) {
            if(BitMatrix::test(row, member)) return true;
        }

        return false;
    }

    void mergeClasses(U32 x, U32 y) {
        BitMatrix::unionInto(scratch.interferes.row(x), scratch.interferes.row(y),
                             scratch.interferes.words());

        for(auto member: scratch.classMembers[y]) scratch.classMembers[x].push(member);
        scratch.classMembers[y].clear();
        parent[y] = x;
    }

    // Every block-argument value one instruction reads. Places are walked beside the operand list
    // for `opt_alias.cpp`'s reason: a place's owner is a use the operand list does not carry.
    template<class F> void eachArgumentUse(JsInst& inst, F&& f) {
        forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
            if(used) f(base[used]->id.index);
        });

        auto placeId = JsPlaceId();
        if(inst.kind == JsInst::Load) placeId = ((JsInstLoad&)inst).place;
        else if(inst.kind == JsInst::Store) placeId = ((JsInstStore&)inst).place;
        else return;

        if(!placeId.valid()) return;

        auto& place = *body.place(base, placeId);
        if(place.owner) f(base[place.owner]->id.index);
        if(place.index) f(base[place.index]->id.index);
        if(place.bitScale) f(base[place.bitScale]->id.index);
    }

    /*
     * The answer, as a value id per value id - the *representative* block argument of the class.
     *
     * A value id rather than the dense index, because that is what the emitter names the variable
     * after and the dense numbering is this file's own. Filled on every path out of `run` that got as
     * far as numbering the arguments, so a body the limits refused reads back "every argument is its
     * own class" rather than nothing.
     */
    void publish() {
        for(U32 index = 0; index < argTotal; index++) {
            target.coalesced.root[valueId[index]] = valueId[classOf(index)];
        }
    }

    void run() {
        auto count = body.blocks.size();
        auto entry = base[body.entry];

        dense.clear();
        dense.reserve(body.values.size());
        for(U32 i = 0; i < body.values.size(); i++) dense.push(kNoArgument);

        valueId.clear();
        parent.clear();

        for(U32 i = 0; i < count; i++) {
            auto block = blockAt(i);
            if(block == entry || block->deleted) continue;

            for(U32 j = 0; j < block->argumentCount(); j++) {
                auto id = base[block->argumentAt(base, j)]->id.index;
                dense[id] = valueId.size();
                parent.push(valueId.size());
                valueId.push(id);
            }
        }

        argTotal = valueId.size();
        if(!argTotal) return;

        // The matrix and the liveness sets are both quadratic in something; a body that would need
        // more than this is emitted the way it was before, which is a refusal and not a wrong
        // answer.
        if(U64(argTotal) * argTotal > kArgumentLimit) return publish();
        if(U64(count) * argTotal > kArgumentLimit) return publish();

        // One bit per (block, argument), on storage the widest body so far already grew - see
        // `Assembly::argUp`. `use` and the arguments are computed once; the fixpoint below moves
        // only `up` and `outOf`.
        auto& up = scratch.up;
        auto& liveAt = scratch.liveAt;
        auto& outOf = scratch.outOf;
        auto& used = scratch.used;
        auto& defined = scratch.defined;

        up.reset(count, argTotal);
        liveAt.reset(count, argTotal);
        outOf.reset(count, argTotal);
        used.reset(count, argTotal);
        defined.reset(count, argTotal);

        auto words = up.words();

        for(U32 b = 0; b < count; b++) {
            auto block = blockAt(b);

            for(auto pointer: block->instructions(base)) {
                eachArgumentUse(*base[pointer], [&](U32 id) {
                    if(id < dense.size() && dense[id] != kNoArgument) {
                        BitMatrix::set(used.row(b), dense[id]);
                    }
                });
            }

            if(auto terminator = block->terminator(base)) {
                eachArgumentUse(*terminator, [&](U32 id) {
                    if(id < dense.size() && dense[id] != kNoArgument) {
                        BitMatrix::set(used.row(b), dense[id]);
                    }
                });
            }
        }

        // An argument is written on the edge into its own block, so it is defined there and never
        // flows to a predecessor. Held as a mask rather than cleared after the fact: clearing it
        // after setting it is a fixpoint that never reaches one, which is what the first version of
        // this did and what made a `Base.js` build not finish.
        for(U32 b = 0; b < count; b++) {
            auto block = blockAt(b);

            for(U32 j = 0; j < block->argumentCount(); j++) {
                auto id = base[block->argumentAt(base, j)]->id.index;
                if(id < dense.size() && dense[id] != kNoArgument) {
                    BitMatrix::set(defined.row(b), dense[id]);
                }
            }
        }

        scratch.row.clear();
        for(Size i = 0; i < words; i++) scratch.row.push(0);

        auto running = scratch.row.pointer();

        auto again = true;
        while(again) {
            again = false;

            for(U32 b = count; b > 0; b--) {
                auto block = blockAt(b - 1);
                auto outRow = outOf.row(b - 1);

                if(auto terminator = block->terminator(base)) {
                    forEachEdge(base, *terminator, [&](JsEdge& edge) {
                        if(!edge.target) return;

                        auto to = base[edge.target];
                        if(BitMatrix::unionInto(outRow, up.row(to->index.index), words)) again = true;

                        for(U32 i = 0; i < edge.arguments.size(); i++) {
                            auto supplied = edge.arguments.get(base, i);
                            if(!supplied) continue;

                            auto id = base[supplied]->id.index;
                            if(id >= dense.size() || dense[id] == kNoArgument) continue;
                            if(BitMatrix::test(outRow, dense[id])) continue;

                            BitMatrix::set(outRow, dense[id]);
                            again = true;
                        }
                    });
                }

                // `up |= (used | outOf) & ~defined`, a word at a time - which is the whole of what
                // the byte-per-column form spelled as a loop over the columns.
                auto usedRow = used.row(b - 1);
                auto definedRow = defined.row(b - 1);
                for(Size w = 0; w < words; w++) {
                    running[w] = (usedRow[w] | outRow[w]) & ~definedRow[w];
                }

                if(BitMatrix::unionInto(up.row(b - 1), running, words)) again = true;
            }
        }

        for(U32 b = 0; b < count; b++) {
            auto block = blockAt(b);
            auto row = liveAt.row(b);

            BitMatrix::copy(row, up.row(b), words);
            BitMatrix::unionInto(row, outOf.row(b), words);

            for(U32 j = 0; j < block->argumentCount(); j++) {
                auto id = base[block->argumentAt(base, j)]->id.index;
                if(id < dense.size() && dense[id] != kNoArgument) {
                    BitMatrix::set(row, dense[id]);
                }
            }
        }

        scratch.interferes.reset(argTotal, argTotal);

        auto mark = [&](U32 x, U32 y) {
            if(x == y) return;
            BitMatrix::set(scratch.interferes.row(x), y);
            BitMatrix::set(scratch.interferes.row(y), x);
};

    // Over the live *list* of each block rather than over every pair of arguments: a body of
    // four hundred joins has a handful live at any one of them, and the square of the total is
    // what made the first version quadratic in something it did not need to be.
    auto& live = scratch.live;
        live.clear();

    for(U32 b = 0; b < count; b++) {
        live.clear();
        if(blockAt(b)->deleted) continue;
        BitMatrix::forEach(liveAt.row(b), words, [&](Size column) { live.push(U32(column)); });

        for(Size i = 0; i < live.size(); i++) {
            for(Size j = i + 1; j < live.size(); j++) mark(live[i], live[j]);
        }
    }

    // The sequential-copy rule, and the copies worth taking, in one walk over the edges.
    auto& candidates = scratch.candidates;
candidates.clear();

    for(U32 b = 0; b < count; b++) {
        if(blockAt(b)->deleted) continue;
        auto terminator = blockAt(b)->terminator(base);
        if(!terminator) continue;

        forEachEdge(base, *terminator, [&](JsEdge& edge) {
            if(!edge.target) return;

            auto to = base[edge.target];

            for(U32 i = 0; i < edge.arguments.size() && i < to->argumentCount(); i++) {
                auto supplied = edge.arguments.get(base, i);
                if(!supplied) continue;

                auto sourceId = base[supplied]->id.index;
                if(sourceId >= dense.size() || dense[sourceId] == kNoArgument) continue;

                auto source = dense[sourceId];

                for(U32 j = 0; j < to->argumentCount(); j++) {
                    auto destId = base[to->argumentAt(base, j)]->id.index;
                    if(destId >= dense.size() || dense[destId] == kNoArgument) continue;
                    if(j != i) mark(dense[destId], source);
                    else { candidates.push(dense[destId]); candidates.push(source); }
                }
            }
        });
    }

    scratch.classMembers.reset(argTotal);
    for(U32 i = 0; i < argTotal; i++) scratch.classMembers[i].push(i);

    for(Size i = 0; i + 1 < candidates.size(); i += 2) {
        auto x = classOf(candidates[i]);
        auto y = classOf(candidates[i + 1]);
        if(x == y || classesInterfere(x, y)) continue;

        mergeClasses(x, y);
    }

    publish();
    coalesceEdgeValues();
}

    /*
     * J90 - an instruction result whose only use is an edge copy into a block argument, computed
     * straight into the argument's variable. The golden-gap analysis counted 532 of
     * `var v673 = p5 + v662 | 0; ... p5 = v673;` in `Base.js`; the peephole in `codegen/js/opt.cpp`
     * folds such a `var` into its use only where nothing stands between them, and in a loop body
     * something usually does.
     *
     * The classes above are about block arguments only, and a result is not given a class here:
     * this is narrower than coalescing it, because it has to be, since a result is written *inside*
     * a block where an argument is written on an edge. So the condition is stated on the one block
     * that holds both the definition and the copy, and is exact there rather than a block-granular
     * interference bit:
     *
     *  - **nothing after the definition reads the argument's class** - no later instruction, not
     *    the terminator's own operand, and no other copy on any of the terminator's edges. The last
     *    is the parallel-copy rule the sequential edge rule above states for arguments: in
     *    `p = v; q = p;` for a swap, writing `v` into `p` early would hand `q` the new value. An
     *    `alias` is followed to what it names, because the emitter spells one as its source;
     *  - **the class is live out of the block along no edge** - where a branch copies `v` into `p`
     *    on one edge, the other edge must not read `p` either;
     *  - **one early assignment per class per block**, since two of them into one variable would be
     *    the second overwriting the first before either edge.
     *
     * Reading the argument *in* the definition is allowed - `p = p + 1` reads before it writes.
     */
    void coalesceEdgeValues() {
        auto& uses = scratch.valueUses;
        auto& aliasOf = scratch.aliasOf;
        auto& claimed = scratch.claimed;

        uses.clear();
        aliasOf.clear();
        for(U32 i = 0; i < body.values.size(); i++) {
            uses.push(0);
            aliasOf.push(kNoArgument);
        }

        auto count = body.blocks.size();
        for(U32 b = 0; b < count; b++) {
            auto block = blockAt(b);
            if(block->deleted) continue;

            auto countUse = [&](U32 id) { if(id < uses.size()) uses[id]++; };

            for(auto pointer: block->instructions(base)) {
                auto& inst = *base[pointer];
                eachArgumentUse(inst, countUse);

                if(inst.kind == JsInst::Alias && inst.result && ((JsInstAlias&)inst).from) {
                    aliasOf[base[inst.result]->id.index] = base[((JsInstAlias&)inst).from]->id.index;
                }
            }

            if(auto terminator = block->terminator(base)) {
                eachArgumentUse(*terminator, countUse);
                forEachEdge(base, *terminator, [&](JsEdge& edge) {
                    for(U32 i = 0; i < edge.arguments.size(); i++) {
                        if(auto supplied = edge.arguments.get(base, i)) countUse(base[supplied]->id.index);
                    }
                });
            }
        }

        // The class a read of `id` is a read of, following aliases; `kNoArgument` for no class.
        auto classRead = [&](U32 id) -> U32 {
            for(U32 guard = 0; guard < 64 && id < aliasOf.size() && aliasOf[id] != kNoArgument; guard++) {
                id = aliasOf[id];
            }

            if(id >= dense.size() || dense[id] == kNoArgument) return kNoArgument;
            return classOf(dense[id]);
        };

        /*
         * Where each value is defined and where each class is last read, per block, in one walk -
         * so "nothing after the definition reads the class" is a comparison rather than a scan of
         * the block per edge argument, which was quadratic in a long loop body.
         */
        auto& definedAt = scratch.definedAt;
        auto& definedIn = scratch.definedIn;
        auto& lastRead = scratch.lastRead;
        auto& lastReadIn = scratch.lastReadIn;
        definedAt.clear();
        definedIn.clear();
        for(U32 i = 0; i < body.values.size(); i++) {
            definedAt.push(0);
            definedIn.push(0);
        }

        lastRead.clear();
        lastReadIn.clear();
        for(U32 i = 0; i < dense.size(); i++) {
            lastRead.push(0);
            lastReadIn.push(0);
        }

        for(U32 b = 0; b < count; b++) {
            auto block = blockAt(b);
            auto terminator = block->terminator(base);
            if(!terminator) continue;

            claimed.clear();

            U32 position = 0;
            for(auto pointer: block->instructions(base)) {
                auto& inst = *base[pointer];
                eachArgumentUse(inst, [&](U32 id) {
                    auto read = classRead(id);
                    if(read >= lastRead.size()) return;
                    lastRead[read] = position;
                    lastReadIn[read] = b + 1;
                });

                if(inst.result && base[inst.result]->id.index < definedAt.size()) {
                    auto id = base[inst.result]->id.index;
                    if(definedIn[id] != b + 1) {
                        definedAt[id] = position;
                        definedIn[id] = b + 1;
                    }
                }

                position++;
            }

            forEachEdge(base, *terminator, [&](JsEdge& copyEdge) {
                if(!copyEdge.target) return;
                auto to = base[copyEdge.target];

                for(U32 i = 0; i < copyEdge.arguments.size() && i < to->argumentCount(); i++) {
                    auto supplied = copyEdge.arguments.get(base, i);
                    if(!supplied) continue;

                    auto valueIdOf = base[supplied]->id.index;
                    auto destId = base[to->argumentAt(base, i)]->id.index;
                    if(valueIdOf >= dense.size() || dense[valueIdOf] != kNoArgument) continue;
                    if(destId >= dense.size() || dense[destId] == kNoArgument) continue;
                    if(uses[valueIdOf] != 1 || aliasOf[valueIdOf] != kNoArgument) continue;

                    auto destClass = classOf(dense[destId]);

                    auto isClaimed = false;
                    for(auto c: claimed) if(c == destClass) isClaimed = true;
                    if(isClaimed) continue;

                    // Defined in this block, and nothing after the definition reads the class. A
                    // read inside the definition itself is allowed.
                    if(valueIdOf >= definedIn.size() || definedIn[valueIdOf] != b + 1) continue;
                    if(destClass < lastRead.size() && lastReadIn[destClass] == b + 1 &&
                       lastRead[destClass] > definedAt[valueIdOf]) {
                        continue;
                    }

                    auto clear = true;

                    eachArgumentUse(*terminator, [&](U32 id) { if(classRead(id) == destClass) clear = false; });
                    forEachEdge(base, *terminator, [&](JsEdge& other) {
                        for(U32 k = 0; k < other.arguments.size(); k++) {
                            auto read = other.arguments.get(base, k);
                            if(!read || (&other == &copyEdge && k == i)) continue;
                            if(classRead(base[read]->id.index) == destClass) clear = false;
                        }
                    });

                    if(!clear) continue;

                    auto outRow = scratch.outOf.row(b);
                    for(auto member: scratch.classMembers[destClass]) {
                        if(BitMatrix::test(outRow, member)) clear = false;
                    }

                    if(!clear) continue;

                    claimed.push(destClass);
                    target.coalesced.into[valueIdOf] = destId;
                }
            });
        }
    }
};

} // namespace

const JsCoalesced& jsCoalesceArguments(JsTarget& target, JsBody& body) {
    auto& answer = target.coalesced;

    answer.root.clear();
    answer.into.clear();
    for(U32 i = 0; i < body.values.size(); i++) {
        answer.root.push(JsCoalesced::kNoClass);
        answer.into.push(JsCoalesced::kNoClass);
    }

    Coalescer coalescer(target, body);
    coalescer.run();
    return answer;
}

} // namespace jsir
