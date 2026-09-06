#include "dominance.h"

namespace ssa {

void reversePostorder(SsaBase base, Body& body, Array<U32>& order) {
    order.clear();

    auto count = body.blocks.size();
    if(!count) return;

    SmallArray<U32, 64> stack, next, postorder;
    IndexSet seen;

    seen.reset(count);
    for(U32 i = 0; i < count; i++) next.push(0);

    auto entry = base[body.entry]->index.index;
    stack.push(entry);
    seen.set(entry, true);

    while(stack.size()) {
        auto index = stack[stack.size() - 1];
        auto block = body.block(base, BlockId(index));

        SmallArray<U32, 4> successors;
        forEachSuccessor(base, *block, [&](U32 target) { successors.push(target); });

        if(next[index] < successors.size()) {
            auto successor = successors[next[index]++];
            if(seen.add(successor)) stack.push(successor);
            continue;
        }

        postorder.push(index);
        stack.pop();
    }

    for(Size i = postorder.size(); i > 0; i--) order.push(postorder[i - 1]);
}

void Dominators::build(SsaBase base, Body& body) {
    auto count = body.blocks.size();

    position.clear();
    idom.clear();

    for(U32 i = 0; i < count; i++) { position.push(kNone); idom.push(kNone); }

    reversePostorder(base, body, order);
    if(!count) return;

    for(U32 i = 0; i < order.size(); i++) position[order[i]] = i;

    auto entry = base[body.entry]->index.index;
    idom[entry] = entry;

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto index: order) {
            if(index == entry) continue;

            auto block = body.block(base, BlockId(index));
            U32 candidate = kNone;

            for(auto predecessorPointer: block->incoming(base)) {
                auto predecessor = base[predecessorPointer]->index.index;
                if(position[predecessor] == kNone || idom[predecessor] == kNone) continue;

                candidate = candidate == kNone ? predecessor : intersect(candidate, predecessor);
            }

            if(candidate != kNone && idom[index] != candidate) {
                idom[index] = candidate;
                changed = true;
            }
        }
    }

    numberTree(count, entry);
}

void Dominators::numberTree(U32 count, U32 entry) {
    childStart.clear();
    childList.clear();
    enter.clear();
    leave.clear();

    for(U32 i = 0; i <= count; i++) childStart.push(0);
    for(U32 i = 0; i < count; i++) { enter.push(0); leave.push(0); }

    for(auto index: order) {
        if(index == entry) continue;
        childStart[idom[index] + 1]++;
    }

    for(U32 i = 0; i < count; i++) childStart[i + 1] += childStart[i];
    for(U32 i = 0; i < childStart[count]; i++) childList.push(0);

    SmallArray<U32, 64> filled;
    for(U32 i = 0; i < count; i++) filled.push(0);

    for(auto index: order) {
        if(index == entry) continue;

        auto parent = idom[index];
        childList[childStart[parent] + filled[parent]++] = index;
    }

    // The same explicit stack, for the same reason. A block is stamped on the way down and closed on
    // the way up, so the interval it spans is exactly its subtree.
    SmallArray<U32, 64> stack, cursor;
    for(U32 i = 0; i < count; i++) cursor.push(0);

    U32 clock = 0;
    stack.push(entry);
    enter[entry] = clock++;

    while(stack.size()) {
        auto index = stack[stack.size() - 1];
        auto first = childStart[index];
        auto last = childStart[index + 1];

        if(first + cursor[index] < last) {
            auto child = childList[first + cursor[index]++];
            stack.push(child);
            enter[child] = clock++;
            continue;
        }

        leave[index] = clock++;
        stack.pop();
    }
}

} // namespace ssa
