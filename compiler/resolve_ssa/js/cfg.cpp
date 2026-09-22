#include "cfg.h"

namespace jsir {

namespace {

constexpr U32 kNoBlock = JsCfg::kNoBlock;

struct Builder {
    Builder(JsTarget& target, JsBody& body): target(target), body(body), base(*target.arena),
                                             cfg(target.cfg) {}

    JsTarget& target;
    JsBody& body;
    JsirBase base;
    JsCfg& cfg;

    Size count = 0;

    // The successors of every block, in edge order - which is the order the walk below explores
    // them in and therefore the order the whole numbering comes out of.
    void buildSuccessors() {
        // `reserve` ahead of every fill in this file, for `JsConvertScratch::reset`'s reason: the
        // count is known and `push` alone climbs to it one doubling at a time.
        cfg.succStart.clear();
        cfg.succStart.reserve(U32(count + 1));
        for(Size i = 0; i <= count; i++) cfg.succStart.push(0);

        auto each = [&](Size index, auto&& f) {
            auto block = body.block(base, JsBlockId(index));
            if(auto terminator = block->terminator(base)) {
                forEachEdge(base, *terminator, [&](JsEdge& edge) {
                    if(edge.target) f(base[edge.target]->index.index);
                });
            }
        };

        for(Size i = 0; i < count; i++) each(i, [&](U32) { cfg.succStart[i + 1]++; });
        for(Size i = 0; i < count; i++) cfg.succStart[i + 1] += cfg.succStart[i];

        cfg.succList.clear();
        cfg.succList.reserve(cfg.succStart[count]);
        for(U32 i = 0; i < cfg.succStart[count]; i++) cfg.succList.push(0);

        for(Size i = 0; i < count; i++) {
            auto fill = cfg.succStart[i];
            each(i, [&](U32 to) { cfg.succList[fill++] = to; });
        }
    }

    /*
     * Reverse postorder, extended over the blocks the entry cannot reach.
     *
     * The extension costs nothing - the structuring never arrives at such a block - and it is what
     * makes `rpo` a total order, which is what the nesting of two merge regions is decided by.
     */
    void computeOrder() {
        cfg.rpo.clear();
        cfg.rpo.reserve(U32(count));
        for(Size i = 0; i < count; i++) cfg.rpo.push(kNoBlock);

        cfg.order.clear();
        if(!count || !body.entry) {
            cfg.reachedCount = 0;
            return;
        }

        auto& stack = cfg.walkStack;
        auto& cursor = cfg.walkCursor;
        auto& postorder = cfg.walkPostorder;
        auto& seen = cfg.walkSeen;

        stack.clear();
        cursor.clear();
        postorder.clear();
        seen.reset(count);

        auto entry = base[body.entry]->index.index;
        seen.set(entry, true);
        stack.push(entry);
        cursor.push(0);

        while(stack.size()) {
            auto top = stack.size() - 1;
            auto block = stack[top];

            if(cursor[top] < cfg.successorCount(block)) {
                auto child = cfg.successorAt(block, cursor[top]++);
                if(!seen[child]) {
                    seen.set(child, true);
                    stack.push(child);
                    cursor.push(0);
                }

                continue;
            }

            postorder.push(block);
            stack.pop();
            cursor.pop();
        }

        U32 position = 0;
        for(Size i = postorder.size(); i > 0; i--) {
            cfg.rpo[postorder[i - 1]] = position++;
            cfg.order.push(postorder[i - 1]);
        }

        cfg.reachedCount = position;
        for(Size i = 0; i < count; i++) if(cfg.rpo[i] == kNoBlock) cfg.rpo[i] = position++;
    }

    /*
     * Immediate dominators, by the Cooper-Harvey-Kennedy fixpoint over the reverse postorder.
     *
     * The structurer used to compute the whole dominator *set* of every block and scan for the
     * largest proper dominator to get this back out of it, which is the same answer at O(n^2) bits
     * and an O(n^2) scan on top. The two queries it asked of the sets - "does this successor
     * dominate me" and "does the header dominate me" - are the chain walk `JsCfg::dominates` is.
     */
    void computeDominators() {
        cfg.idom.clear();
        cfg.idom.reserve(U32(count));
        for(Size i = 0; i < count; i++) cfg.idom.push(kNoBlock);
        if(!cfg.order.size()) return;

        auto entry = cfg.order[0];
        cfg.idom[entry] = entry;

        auto intersect = [&](U32 a, U32 b) {
            while(a != b) {
                while(cfg.rpo[a] > cfg.rpo[b]) a = cfg.idom[a];
                while(cfg.rpo[b] > cfg.rpo[a]) b = cfg.idom[b];
            }

            return a;
        };

        for(auto changed = true; changed;) {
            changed = false;

            for(auto at: cfg.order) {
                if(at == entry) continue;

                auto fresh = kNoBlock;
                for(auto pointer: body.block(base, JsBlockId(at))->incoming(base)) {
                    if(!pointer) continue;

                    auto from = base[pointer]->index.index;
                    if(from >= count || cfg.idom[from] == kNoBlock) continue;
                    if(cfg.rpo[from] >= cfg.reachedCount) continue;

                    fresh = fresh == kNoBlock ? from : intersect(from, fresh);
                }

                if(fresh != kNoBlock && cfg.idom[at] != fresh) { cfg.idom[at] = fresh; changed = true; }
            }
        }

        // The entry dominates itself and nothing above it - `kNoBlock` is what the chain walk stops
        // on, and leaving `entry` pointing at itself would make `dominates` loop.
        cfg.idom[entry] = kNoBlock;
    }

    /*
     * Which blocks are loop headers, how many *forward* edges arrive at each block, and the natural
     * loop of every header.
     *
     * A back edge is an edge whose target dominates its source, which is the definition rather than
     * an approximation of one; everything else arriving at a block is a way forward into it.
     */
    void computeLoops() {
        cfg.loopHeader.reset(count);
        cfg.forwardPreds.clear();
        cfg.forwardPreds.reserve(U32(count));
        for(Size i = 0; i < count; i++) cfg.forwardPreds.push(0);

        for(Size i = 0; i < count; i++) {
            if(!cfg.reachable[i]) continue;

            for(U32 s = 0; s < cfg.successorCount(U32(i)); s++) {
                auto to = cfg.successorAt(U32(i), s);
                if(cfg.dominates(to, U32(i))) cfg.loopHeader.set(to, true);
                else cfg.forwardPreds[to]++;
            }
        }

        cfg.loops.reset(count, count);
        cfg.loopMask.clear();
        cfg.loopMask.reserve(U32(count));
        for(Size i = 0; i < count; i++) cfg.loopMask.push(0);

        cfg.loopCount = 0;
        cfg.reducible = true;

        // A retreating edge whose target does not dominate its source names a cycle with more than
        // one entry, which no set of natural loops describes - see `JsCfg::reducible`.
        for(Size i = 0; i < count; i++) {
            if(!cfg.reachable[i]) continue;

            for(U32 s = 0; s < cfg.successorCount(U32(i)); s++) {
                auto to = cfg.successorAt(U32(i), s);
                if(!cfg.reachable[to] || cfg.rpo[to] > cfg.rpo[i]) continue;
                if(!cfg.dominates(to, U32(i))) cfg.reducible = false;
            }
        }

        for(Size header = 0; header < count; header++) {
            if(!cfg.loopHeader[header]) continue;

            naturalLoop(U32(header), cfg.loops[header]);

            if(cfg.loopCount >= 64) { cfg.reducible = false; continue; }

            auto bit = U64(1) << cfg.loopCount++;
            for(Size block = 0; block < count; block++) {
                if(cfg.loops[header][block]) cfg.loopMask[block] |= bit;
            }
        }
    }

    // The header, and everything that reaches a back edge's source without leaving through it.
    void naturalLoop(U32 header, IndexSet& into) {
        into.reset(count);
        into.set(header, true);

        auto& pending = cfg.walkStack;
        pending.clear();

        for(Size i = 0; i < count; i++) {
            if(!cfg.reachable[i] || !cfg.dominates(header, U32(i))) continue;

            for(U32 s = 0; s < cfg.successorCount(U32(i)); s++) {
                if(cfg.successorAt(U32(i), s) == header) pending.push(U32(i));
            }
        }

        while(pending.size()) {
            auto block = pending[pending.size() - 1];
            pending.pop();

            if(into[block]) continue;
            into.set(block, true);

            for(auto pointer: body.block(base, JsBlockId(block))->incoming(base)) {
                if(!pointer) continue;

                auto from = base[pointer]->index.index;
                if(!into[from]) pending.push(from);
            }
        }
    }

    void run() {
        count = body.blocks.size();

        buildSuccessors();
        computeOrder();

        cfg.reachable.reset(count);
        for(Size i = 0; i < count; i++) cfg.reachable.set(i, cfg.rpo[i] < cfg.reachedCount);

        computeDominators();
        computeLoops();

        cfg.ready = true;
    }
};

} // namespace

const JsCfg& jsCfgOf(JsTarget& target, JsBody& body) {
    if(target.cfg.ready) return target.cfg;

    Builder builder(target, body);
    builder.run();
    return target.cfg;
}

} // namespace jsir
