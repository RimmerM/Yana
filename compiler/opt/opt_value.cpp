#include "opt_pass.h"

/*
 * The two passes that need to know where a value is *available* rather than only what it is.
 *
 * Common-subexpression elimination replaces a computation with an earlier one, which is only sound
 * where the earlier one has definitely happened - so it walks the dominator tree with a scope per
 * node, and everything still in scope dominates the instruction being looked at. Dead-value
 * elimination needs no such thing and is here because it is what the other two leave work for.
 *
 * Dominance is the textbook set fixpoint rather than the near-linear algorithm, on the same grounds
 * codegen/js/flow.cpp gives for its copy: a function's blocks are counted in tens, and being
 * obviously correct matters more than being fast when the failure mode is a value used where it has
 * not been computed.
 */

namespace {

struct Dominance {
    // Per block, a byte per block: whether that one dominates this one.
    Array<Array<U8>> dominators;
    Array<U32> immediate;
    Array<Array<U32>> children;
    Array<ModulePtr<Block>> blocks;

    static constexpr U32 kNone = maxLimit<U32>;
};

void computeDominance(OptContext& opt, Dominance& result) {
    auto& blocks = opt.function->blocks;
    auto count = blocks.size();

    for(Size i = 0; i < count; i++) {
        result.blocks.push(blocks.get(opt.local, i));
        result.immediate.push(Dominance::kNone);
        result.children.push(Array<U32>());

        Array<U8> row;
        for(Size j = 0; j < count; j++) row.push(i == 0 ? U8(i == j) : U8(1));
        result.dominators.push(::move(row));
    }

    auto changed = true;
    while(changed) {
        changed = false;

        for(Size i = 1; i < count; i++) {
            auto block = opt.local[result.blocks[i]];

            Array<U8> next;
            auto first = true;

            for(auto predecessor: block->incoming.contents(opt.local)) {
                auto from = opt.local[predecessor]->index;

                if(first) {
                    for(Size j = 0; j < count; j++) next.push(result.dominators[from][j]);
                    first = false;
                } else {
                    for(Size j = 0; j < count; j++) next[j] &= result.dominators[from][j];
                }
            }

            // A block nothing reaches is dominated by itself and nothing else, which keeps it out of
            // every scope below without needing a case of its own.
            if(first) for(Size j = 0; j < count; j++) next.push(0);
            next[i] = 1;

            for(Size j = 0; j < count; j++) {
                if(result.dominators[i][j] == next[j]) continue;

                result.dominators[i][j] = next[j];
                changed = true;
            }
        }
    }

    // The immediate dominator is the one strict dominator that every other strict dominator also
    // dominates, which is the same as the one with the most dominators of its own.
    for(Size i = 1; i < count; i++) {
        auto best = Dominance::kNone;
        Size bestCount = 0;

        for(Size j = 0; j < count; j++) {
            if(i == j || !result.dominators[i][j]) continue;

            Size own = 0;
            for(Size k = 0; k < count; k++) own += result.dominators[j][k];

            if(best != Dominance::kNone && own <= bestCount) continue;

            best = U32(j);
            bestCount = own;
        }

        result.immediate[i] = best;
        if(best != Dominance::kNone) result.children[best].push(U32(i));
    }
}

/*
 * Whether two pure instructions compute the same thing.
 *
 * Operands are compared as SSA values, which is what makes this an equality rather than a
 * congruence: two instructions with equal-but-distinct operands are left alone, and the fixed point
 * catches them on the next round once the operands themselves have been unified.
 *
 * Everything a kind carries beside its operands has to be compared here. A kind whose extra state is
 * not listed would have two instructions declared equal on their operands alone - which is why the
 * default is to decline rather than to accept.
 */
bool sameComputation(OptContext& opt, Value& a, Value& b) {
    if(a.kind != b.kind || a.type != b.type) return false;

    switch(a.kind) {
        case Value::Cast: case Value::Neg: case Value::Not:
            return ((InstUnary&)a).from == ((InstUnary&)b).from;
        case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
        case Value::Shl: case Value::Shr: case Value::Sar:
        case Value::And: case Value::Or: case Value::Xor:
            return ((InstBinary&)a).lhs == ((InstBinary&)b).lhs &&
                   ((InstBinary&)a).rhs == ((InstBinary&)b).rhs;
        case Value::Cmp:
            return ((InstCmp&)a).lhs == ((InstCmp&)b).lhs &&
                   ((InstCmp&)a).rhs == ((InstCmp&)b).rhs &&
                   ((InstCmp&)a).cmp == ((InstCmp&)b).cmp;
        case Value::TypeMetric:
            return ((InstTypeMetric&)a).of == ((InstTypeMetric&)b).of &&
                   ((InstTypeMetric&)a).metric == ((InstTypeMetric&)b).metric;
        case Value::Symbol:
            return ((InstSymbol&)a).callee == ((InstSymbol&)b).callee &&
                   ((InstSymbol&)a).global == ((InstSymbol&)b).global;
        default:
            return false;
    }
}

// One walk of the dominator tree, carrying the expressions computed on the path from the entry to
// the block being visited. Recursive because the depth is the tree's rather than the function's.
void eliminateInBlock(OptContext& opt, Dominance& dominance, U32 index,
                      Array<ModulePtr<Inst>>& available) {
    auto scope = available.size();
    auto block = opt.local[dominance.blocks[index]];

    for(Size i = 0; i < block->instructions.size(); i++) {
        auto pointer = block->instructions.get(opt.local, i);
        auto instruction = opt.local[pointer];
        if(!isPureValue(*instruction)) continue;

        ModulePtr<Inst> existing = nullptr;
        for(Size a = available.size(); a-- > 0;) {
            if(!sameComputation(opt, *opt.local[available[a]], *instruction)) continue;

            existing = available[a];
            break;
        }

        if(existing) {
            replaceValue(opt, (ModulePtr<Value>)pointer, (ModulePtr<Value>)existing);
        } else {
            available.push(pointer);
        }
    }

    for(auto child: dominance.children[index]) eliminateInBlock(opt, dominance, child, available);

    while(available.size() > scope) available.pop();
}

}

void eliminateCommonValues(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    Dominance dominance;
    computeDominance(opt, dominance);

    Array<ModulePtr<Inst>> available;
    eliminateInBlock(opt, dominance, 0, available);
}

/*
 * A read whose result nothing reads.
 *
 * Reading a place has no effect, so removing one is only a question of whether it could have
 * *failed*. A local or a global is storage the checker proved is there; a pointer or a borrow root
 * is an address the program computed, and removing a load through one would remove a fault the
 * program is entitled to take. So the first two go and the last two stay.
 *
 * These exist in quantity rather than as an oddity: the resolver emits a whole-aggregate load in
 * front of every field access - `%v9 = load %e : Entry` before `%v10 = load %e@Entry.live` - and
 * nothing has ever read one.
 */
static bool isDeadRead(OptContext& opt, Value& instruction) {
    if(instruction.kind != Value::LoadPlace) return false;

    auto& place = ((InstLoadPlace&)instruction).place;
    return place.root == PlaceRoot::Local || place.root == PlaceRoot::Global;
}

void eliminateDeadValues(OptContext& opt) {
    /*
     * To a fixed point within this pass rather than across the driver's rounds, because the shape it
     * produces is a chain: the operands of an instruction it removed are exactly the values that may
     * have just become unread, and walking the blocks backwards catches most of that in one sweep.
     *
     * Only the pure kinds. Everything else in the IR either has an effect, is one of the ownership
     * decisions the analyses already took, or reads storage whose writers this pass cannot see.
     */
    auto changed = true;
    while(changed) {
        changed = false;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(Size i = block->instructions.size(); i-- > 0;) {
                auto pointer = block->instructions.get(opt.local, i);
                auto instruction = opt.local[pointer];

                if(instruction->uses.isNotEmpty()) continue;
                if(!isPureValue(*instruction) && !isDeadRead(opt, *instruction)) continue;

                eraseInstruction(opt, pointer);
                changed = true;
            }
        }
    }
}
