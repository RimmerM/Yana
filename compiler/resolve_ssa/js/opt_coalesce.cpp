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
            if(block == entry) continue;

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
        BitMatrix::forEach(liveAt.row(b), words, [&](Size column) { live.push(U32(column)); });

        for(Size i = 0; i < live.size(); i++) {
            for(Size j = i + 1; j < live.size(); j++) mark(live[i], live[j]);
        }
    }

    // The sequential-copy rule, and the copies worth taking, in one walk over the edges.
    auto& candidates = scratch.candidates;
candidates.clear();

    for(U32 b = 0; b < count; b++) {
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
}
};

} // namespace

const JsCoalesced& jsCoalesceArguments(JsTarget& target, JsBody& body) {
    auto& answer = target.coalesced;

    answer.root.clear();
    for(U32 i = 0; i < body.values.size(); i++) answer.root.push(JsCoalesced::kNoClass);

    Coalescer coalescer(target, body);
    coalescer.run();
    return answer;
}

} // namespace jsir
