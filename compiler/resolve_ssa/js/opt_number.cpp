#include "opt_internal.h"

namespace jsir {

/*
 * The second of two identical computations, reads and branches, answered by the first one that
 * dominates it - the redundancy an `@inline(always)` graft leaves once per copy.
 *
 * `xs[i] = {xs[i] | age: xs[i].age + 1, ...}` grafts `Index` four times, and each copy brings
 * its own `if i >= length then checkFailed()` and its own `xs[offset + i]`. OSSA's `cse` sees
 * none of it: a window's `length` is a field of a view record that on native is storage and is
 * read through `load`s it roots nowhere - `cse.cpp` answers only reads rooted in an allocation of
 * the body - and it declines a comparison across blocks on purpose. Here the view is flat values
 * (J71), so the four lengths are **one** JSIR value and the four checks are four `convert`s and
 * four `ge`s over it; what was missing was a pass that says so.
 *
 * Three things, in one walk of the dominator tree:
 *
 *  - **a pure computation** - `binary`, `unary`, `coerce`, `convert` - over the same canonical
 *    operands as one in scope becomes an alias of it. A comparison is numbered but not
 *    rewritten across blocks, for `cse.cpp`'s reason: a second reader in another block is a
 *    materialized `? 1 : 0` where the first was an expression inside its `if`;
 *  - **a read** of a property, an element or a range of either is answered by an identical read
 *    in scope, where scope is narrower than dominance: a block entered from anything but its
 *    one predecessor ends every read, and so does any write that is not a JavaScript local and
 *    every call. A range read of an element some whole read already answered becomes a range of
 *    that value, which is `ageCells`' `v51[i] & 1048575` after `s1 = v51[i]`;
 *  - **a branch** on a condition a dominating arm decided - the arm's block entered only from
 *    that arm - becomes a jump, and the arm it can no longer take is deleted if nothing else
 *    reaches it.
 *
 * Canonical values are what makes the three one pass: the second `ge` is the first once its
 * `convert` is, and the second `xs[offset + i]` is the first once its index is.
 */
U32 Optimizer::canonicalOf(JsValue* value) {
    auto& canonical = s.canonical;
    auto id = value->id.index;
    return id < canonical.size() ? canonical[id] : id;
}

// Whether a place is a JavaScript local, whose write no other read in this body can observe.
bool Optimizer::writesLocal(JsPlaceId id) {
    auto& place = builder.place(id);
    if(place.kind == JsPlaceKind::Var) return true;
    return place.kind == JsPlaceKind::BitRange && builder.place(place.word).kind == JsPlaceKind::Var;
}

bool Optimizer::samePlace(JsPlaceId leftId, JsPlaceId rightId) {
    if(leftId.index == rightId.index) return true;

    auto& left = builder.place(leftId);
    auto& right = builder.place(rightId);
    if(left.kind != right.kind || left.content != right.content) return false;
    if(left.nichePayload != right.nichePayload) return false;

    auto same = [&](JsirPtr<JsValue> a, JsirPtr<JsValue> b) {
        if(!a || !b) return !a && !b;
        return canonicalOf(base[a]) == canonicalOf(base[b]);
    };

    switch(left.kind) {
        case JsPlaceKind::Property:
            return same(left.owner, right.owner) && left.slot == right.slot &&
                   left.shape == right.shape;
        case JsPlaceKind::Element:
            return same(left.owner, right.owner) && same(left.index, right.index);
        case JsPlaceKind::BitRange:
            return left.bitOffset == right.bitOffset && left.bitWidth == right.bitWidth &&
                   left.bitSigned == right.bitSigned && left.bitWhole == right.bitWhole &&
                   same(left.bitScale, right.bitScale) && readable(left.word) &&
                   samePlace(left.word, right.word);
        default:
            return false;
    }
}

// The places a read of which may be answered by another: storage in an object or an array.
bool Optimizer::readable(JsPlaceId id) {
    auto& place = builder.place(id);
    if(place.kind == JsPlaceKind::Property || place.kind == JsPlaceKind::Element) return true;
    return place.kind == JsPlaceKind::BitRange && !place.bitScale &&
           (builder.place(place.word).kind == JsPlaceKind::Property ||
            builder.place(place.word).kind == JsPlaceKind::Element);
}

bool Optimizer::sameComputation(JsInst& a, JsInst& b, bool constants) {
    if(a.kind != b.kind || !a.result || !b.result) return false;
    if(base[a.result]->rep != base[b.result]->rep) return false;

    auto compared = constants && isComparison(a);
    auto same = [&](JsirPtr<JsValue> x, JsirPtr<JsValue> y) {
        if(!x || !y) return false;
        if(compared) return comparedOf(base[x]) == comparedOf(base[y]);
        return canonicalOf(base[x]) == canonicalOf(base[y]);
    };

    switch(a.kind) {
        case JsInst::Binary: {
            auto& l = (JsInstBinary&)a;
            auto& r = (JsInstBinary&)b;
            return l.op == r.op && same(l.lhs, r.lhs) && same(l.rhs, r.rhs);
        }
        case JsInst::Unary: {
            auto& l = (JsInstUnary&)a;
            auto& r = (JsInstUnary&)b;
            return l.op == r.op && same(l.value, r.value);
        }
        case JsInst::Coerce:
            return same(((JsInstCoerce&)a).value, ((JsInstCoerce&)b).value);
        case JsInst::Convert:
            return ((JsInstConvert&)a).fromSigned == ((JsInstConvert&)b).fromSigned &&
                   same(((JsInstConvert&)a).value, ((JsInstConvert&)b).value);
        default:
            return false;
    }
}

// The operand a computation is keyed on - its first, which is what the linked lists hang off.
JsValue* Optimizer::keyOperand(JsInst& inst) {
    switch(inst.kind) {
        case JsInst::Binary: return ((JsInstBinary&)inst).lhs ? base[((JsInstBinary&)inst).lhs] : nullptr;
        case JsInst::Unary: return ((JsInstUnary&)inst).value ? base[((JsInstUnary&)inst).value] : nullptr;
        case JsInst::Coerce: return ((JsInstCoerce&)inst).value ? base[((JsInstCoerce&)inst).value] : nullptr;
        case JsInst::Convert: return ((JsInstConvert&)inst).value ? base[((JsInstConvert&)inst).value] : nullptr;
        default: return nullptr;
    }
}

// A read's key: the owner of the storage it reads, through a range to the word's.
JsValue* Optimizer::keyOwner(JsPlaceId id) {
    auto& place = builder.place(id);
    return place.owner ? base[place.owner] : nullptr;
}

bool Optimizer::isComparison(JsInst& inst) {
    if(inst.kind != JsInst::Binary) return false;
    auto op = ((JsInstBinary&)inst).op;
    return op == mir::BinaryOp::Eq || op == mir::BinaryOp::Ne || op == mir::BinaryOp::Lt ||
           op == mir::BinaryOp::Le || op == mir::BinaryOp::Gt || op == mir::BinaryOp::Ge;
}

// The constant `value` is defined as, or null - the one place a `Const` definer is looked for.
const JsConstant* Optimizer::constantOf(JsValue* value) {
    if(!value || !value->definer) return nullptr;
    auto definer = base[value->definer];
    return definer->kind == JsInst::Const ? &((JsInstConst&)*definer).constant : nullptr;
}

// Whether `value` is the number zero, of either sign - what `0 + i` is asked with.
bool Optimizer::isNumberZero(JsValue* value) {
    auto constant = constantOf(value);
    return constant && constant->kind == JsConstant::Number && constant->number == 0;
}

/*
 * `0 + i` - a window's `offset + i` where the window starts at zero - is `i`, where the sum is
 * an integer of the representation `i` already has. A float is refused: `-0 + 0` is `+0`.
 */
JsValue* Optimizer::identityOf(JsInst& inst) {
    if(inst.kind != JsInst::Binary || !inst.result) return nullptr;

    auto& binary = (JsInstBinary&)inst;
    if(binary.op != mir::BinaryOp::Add || !binary.lhs || !binary.rhs) return nullptr;

    auto& rep = target.rep(base[inst.result]->rep);
    if(!rep.isHostInt32() || rep.domain == NumberDomain::Bool01) return nullptr;

    auto lhs = base[binary.lhs];
    auto rhs = base[binary.rhs];
    if(isNumberZero(lhs) && rhs->rep == base[inst.result]->rep) return rhs;
    if(isNumberZero(rhs) && lhs->rep == base[inst.result]->rep) return lhs;
    return nullptr;
}

/*
 * Two `Const`s of one value at one representation are one canonical value.
 *
 * The conversion writes a literal where it is used, so `if x > 0: if x > 0:` compares `x` with two
 * different zeros, and the second comparison - or a second bounds check against a literal index -
 * was never the first one's. A constant is not *rewritten* into another: it is spelled at every
 * use (`constantOnly`), and what it gains is only that a comparison over it keys the same.
 *
 * Comparisons only, and only as a *fact*: the second comparison takes the first one's canonical
 * value, so a branch on it is decided by the arm above, but it is not rewritten into it. Both
 * limits are measured. Giving every computation the class was the first version, and it made the
 * corpus's JavaScript larger - `x & 15` spelled at two uses became one `var`, and so did `xs[2]`
 * read twice, a kilobyte of `var`s in `VecMove`. Rewriting a comparison in its own block was the
 * second, and `ConstructorMap.Records`' `v === 1` with two readers hoisted two object literals out
 * of the conditional that chose between them.
 *
 * Keyed by a hash of the whole constant and checked field by field, since a collision must refuse
 * rather than merge. A number compares by its bits, so `0` and `-0` stay two, and `integral` is in
 * the key so that two constants spelled differently are never one.
 */
static U64 constantKey(const JsConstant& constant, JsRepId rep) {
    U64 payload = 0;
    switch(constant.kind) {
        case JsConstant::Number:
        case JsConstant::Bool: memcpy(&payload, &constant.number, sizeof(payload)); break;
        case JsConstant::BigInt: payload = constant.bits; break;
        case JsConstant::Text: payload = U32(constant.text); break;
        default: break;
    }

    U64 key = payload ^ (U64(constant.kind) << 56) ^ (U64(rep.index) << 20) ^
              (U64(constant.integral) << 1) ^ U64(constant.isSigned);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    return key == maxLimit<U64> ? 0 : key;
}

static bool sameConstant(const JsConstant& a, const JsConstant& b) {
    if(a.kind != b.kind || a.integral != b.integral || a.isSigned != b.isSigned) return false;
    switch(a.kind) {
        case JsConstant::Number:
        case JsConstant::Bool: return memcmp(&a.number, &b.number, sizeof(a.number)) == 0;
        case JsConstant::BigInt: return a.bits == b.bits;
        case JsConstant::Text: return a.text == b.text;
        default: return true;
    }
}

void Optimizer::numberConstant(JsInst& inst) {
    auto value = base[inst.result];
    auto& constant = ((JsInstConst&)inst).constant;
    auto key = constantKey(constant, value->rep);

    if(auto found = s.constantValues.get(key)) {
        auto first = base[body.values.get(base, found.unwrap())];
        auto other = constantOf(first);
        if(other && first->rep == value->rep && sameConstant(*other, constant)) {
            s.constantClass[value->id.index] = first->id.index;
        }
        return;
    }

    s.constantValues.add(key, value->id.index);
}

// What a comparison's operand is keyed and compared on: its constant's first value, or its
// canonical value.
U32 Optimizer::comparedOf(JsValue* value) {
    auto id = value->id.index;
    auto& constants = s.constantClass;
    return id < constants.size() && constants[id] != maxLimit<U32> ? constants[id] : canonicalOf(value);
}

bool Optimizer::overConstants(JsInst& inst) {
    if(inst.kind == JsInst::Const) return true;
    if(inst.kind != JsInst::Binary && inst.kind != JsInst::Unary && inst.kind != JsInst::Coerce &&
       inst.kind != JsInst::Convert) {
        return false;
    }

    auto all = true;
    forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
        if(!used || !s.constantOnly[base[used]->id.index]) all = false;
    });
    return all;
}

void Optimizer::pushNumbered(JsInst& inst, JsValue* key, bool read) {
    auto& heads = s.numberedHead;
    auto keyId = canonicalOf(key);

    JsOptNumbered entry;
    entry.inst = &inst;
    entry.key = keyId;
    entry.previous = heads[keyId];
    entry.read = read;

    heads[keyId] = U32(s.numbered.size());
    s.numbered.push(entry);
}

void Optimizer::popNumbered(Size to) {
    auto& heads = s.numberedHead;
    while(s.numbered.size() > to) {
        auto entry = s.numbered.pop().unwrap();
        heads[entry.key] = entry.previous;
    }
}

/*
 * Whether `inst` may change storage some other name in this body reads: a store that is not a
 * JavaScript local, any other write, and any call, which may run code this body cannot see.
 *
 * The one answer to "writes or calls" for every pass here. Each of value numbering's read scope,
 * `readThroughCopies`'s window and `updateAsLiteral`'s path asked it separately, and two of them
 * called a bit range over a local a heap write where the third called it local.
 */
bool Optimizer::clobbersHeap(JsInst& inst) {
    if(inst.kind == JsInst::Store) return !writesLocal(((JsInstStore&)inst).place);

    auto effects = effectsOf(inst);
    return (effects & (kJsWrites | kJsCalls)) != 0;
}

/*
 * A range of an element or a property whose whole word a read in scope already produced, as a
 * range of that value - a `Value` place under the same `BitRange`. The form exists already: a
 * field of a packed record that arrived as a value is read that way (`convert.cpp`).
 */
bool Optimizer::readRangeOfValue(JsInst& inst, JsValue* word) {
    auto& old = builder.place(((JsInstLoad&)inst).place);

    JsPlace range = old;
    range.word = builder.addValuePlace(word, word->rep);
    range.owner = word - base;
    range.index = nullptr;
    range.slot = 0;
    range.shape = JsShapeId();
    auto rangeId = builder.addPlace(range);

    release(inst);
    ((JsInstLoad&)inst).place = rangeId;

    while(placeRefs.size() < body.places.size()) placeRefs.push(0);
    placeRefs[rangeId.index]++;
    if(word->id.index < liveUses.size()) liveUses[word->id.index]++;

    return true;
}

void Optimizer::numberBlock(U32 index, U32& barrier) {
    auto block = body.block(base, JsBlockId(index));

    for(auto pointer: block->instructions(base)) {
        auto& inst = *base[pointer];

        if(inst.kind == JsInst::Alias && inst.result) {
            auto from = base[((JsInstAlias&)inst).from];
            s.canonical[base[inst.result]->id.index] = canonicalOf(from);
            if(s.constantOnly[from->id.index]) s.constantOnly.set(base[inst.result]->id.index, true);
            continue;
        }

        if(auto same = identityOf(inst)) {
            s.canonical[base[inst.result]->id.index] = canonicalOf(same);
            pending.push(Rewrite { &inst, same });
            target.optimizer.numbered++;
            continue;
        }

        // A computation over constants alone is spelled where it is used rather than
        // computed, so a second reader is what would give it a `var` - `cse.cpp`'s
        // `materializedAtUse`, and `Scalars.yana`'s `Math.abs(-2)` is the measurement here.
        if(inst.result && overConstants(inst)) {
            if(inst.kind == JsInst::Const) numberConstant(inst);
            s.constantOnly.set(base[inst.result]->id.index, true);
            continue;
        }

        if(auto key = keyOperand(inst); key && inst.result) {
            if(isComparison(inst)) key = base[body.values.get(base, comparedOf(key))];
            auto earlier = findNumbered(key, false, 0, [&](JsInst& other) {
                return sameComputation(other, inst, false);
            });

            // A comparison equal to one in scope only through its constants is *known* - a branch
            // on it is decided below - and is not rewritten into it, since that would give a
            // `? 1 : 0` a second reader and so a `var` (see `numberConstant`).
            if(!earlier && isComparison(inst)) {
                auto known = findNumbered(key, false, 0, [&](JsInst& other) {
                    return sameComputation(other, inst, true);
                });

                if(known) {
                    s.canonical[base[inst.result]->id.index] = canonicalOf(base[known->result]);
                    continue;
                }
            }

            if(earlier) {
                s.canonical[base[inst.result]->id.index] = canonicalOf(base[earlier->result]);

                // A comparison in another block keeps its own node - see above.
                if(!isComparison(inst) || earlier->block == inst.block) {
                    pending.push(Rewrite { &inst, base[earlier->result] });
                    target.optimizer.numbered++;
                }
                continue;
            }

            pushNumbered(inst, key, false);
            continue;
        }

        if(inst.kind == JsInst::Load && inst.result && readable(((JsInstLoad&)inst).place)) {
            auto placeId = ((JsInstLoad&)inst).place;
            auto owner = keyOwner(placeId);
            if(!owner) continue;

            auto earlier = findNumbered(owner, true, barrier, [&](JsInst& other) {
                return base[other.result]->rep == base[inst.result]->rep &&
                       samePlace(((JsInstLoad&)other).place, placeId);
            });

            if(earlier) {
                s.canonical[base[inst.result]->id.index] = canonicalOf(base[earlier->result]);
                pending.push(Rewrite { &inst, base[earlier->result] });
                target.optimizer.numbered++;
                continue;
            }

            // A range whose word an earlier read produced whole.
            auto& place = builder.place(placeId);
            if(place.kind == JsPlaceKind::BitRange) {
                auto word = findNumbered(owner, true, barrier, [&](JsInst& other) {
                    return base[other.result]->rep == builder.place(place.word).content &&
                           samePlace(((JsInstLoad&)other).place, place.word);
                });

                if(word && readRangeOfValue(inst, base[word->result])) {
                    target.optimizer.numbered++;
                    continue;
                }
            }

            pushNumbered(inst, owner, true);
            continue;
        }

        if(inst.kind == JsInst::Branch) {
            auto& branch = (JsInstBranch&)inst;
            if(!branch.condition) continue;

            auto condition = canonicalOf(base[branch.condition]);
            for(Size f = s.facts.size(); f-- > 0;) {
                if(s.facts[f].value != condition) continue;
                s.decisions.push(JsOptDecision { &inst, s.facts[f].answer });
                break;
            }
            continue;
        }

        if(clobbersHeap(inst)) barrier = U32(s.numbered.size());
    }
}

/*
 * What an arm proves on the way into a child: the child's one way in is that arm of the
 * parent's branch, so the parent's condition is decided everywhere the child dominates.
 */
void Optimizer::enterArm(U32 parent, U32 child) {
    auto block = body.block(base, JsBlockId(child));
    if(block->predecessorCount() != 1) return;

    auto end = body.block(base, JsBlockId(parent))->terminator(base);
    if(!end || end->kind != JsInst::Branch) return;

    auto& branch = (JsInstBranch&)*end;
    if(!branch.condition || branch.whenTrue.target == branch.whenFalse.target) return;

    auto into = base[branch.whenTrue.target] == block ? 1 :
                base[branch.whenFalse.target] == block ? 0 : -1;
    if(into < 0) return;

    s.facts.push(JsOptFact { canonicalOf(base[branch.condition]), into == 1 });
}

void Optimizer::numberValues() {
    if(!computeDominance()) return;

    auto count = U32(body.blocks.size());
    auto values = body.values.size();

    auto& canonical = s.canonical;
    canonical.clear();
    canonical.reserve(values);
    for(U32 i = 0; i < values; i++) canonical.push(i);

    auto& heads = s.numberedHead;
    heads.clear();
    heads.reserve(values);
    for(U32 i = 0; i < values; i++) heads.push(maxLimit<U32>);

    s.constantOnly.reset(values);
    s.constantValues.clear();
    s.constantClass.clear();
    s.constantClass.reserve(values);
    for(U32 i = 0; i < values; i++) s.constantClass.push(maxLimit<U32>);
    s.numbered.clear();
    s.facts.clear();
    s.decisions.clear();

    // The dominator tree's children, in reverse postorder so that a definition is numbered
    // before a sibling subtree that cannot see it anyway, and a use after its definition.
    auto& start = s.childStart;
    auto& list = s.childList;
    start.clear();
    for(U32 i = 0; i <= count; i++) start.push(0);

    for(U32 p = 0; p < flow->reachedCount; p++) {
        auto b = flow->order[p];
        auto parent = flow->idom[b];
        if(parent != kNoBlock) start[parent + 1]++;
    }

    for(U32 i = 0; i < count; i++) start[i + 1] += start[i];

    auto& fill = s.fill;
    fill.clear();
    for(U32 i = 0; i < count; i++) fill.push(start[i]);

    list.clear();
    for(U32 i = 0; i < start[count]; i++) list.push(0);

    for(U32 p = 0; p < flow->reachedCount; p++) {
        auto b = flow->order[p];
        auto parent = flow->idom[b];
        if(parent != kNoBlock) list[fill[parent]++] = b;
    }

    struct Frame {
        U32 block = 0;
        U32 cursor = 0;
        Size numbered = 0;
        Size facts = 0;
        U32 barrier = 0;
    };

    SmallArray<Frame, 32> stack;
    U32 barrier = 0;

    auto enter = [&](U32 block, U32 parent) {
        Frame frame;
        frame.block = block;
        frame.numbered = s.numbered.size();
        frame.facts = s.facts.size();
        frame.barrier = barrier;
        stack.push(frame);

        // A block entered from anywhere but its one predecessor may follow code that wrote.
        if(parent == kNoBlock || body.block(base, JsBlockId(block))->predecessorCount() != 1) {
            barrier = U32(s.numbered.size());
        }

        if(parent != kNoBlock) enterArm(parent, block);
        numberBlock(block, barrier);
    };

    auto entry = base[body.entry]->index.index;
    enter(entry, kNoBlock);

    while(stack.size()) {
        auto& frame = stack[stack.size() - 1];
        if(start[frame.block] + frame.cursor < start[frame.block + 1]) {
            auto child = list[start[frame.block] + frame.cursor++];
            enter(child, frame.block);
            continue;
        }

        popNumbered(frame.numbered);
        while(s.facts.size() > frame.facts) s.facts.pop();
        barrier = frame.barrier;
        stack.pop();
    }

    applyRewrites();
    decideBranches();
}

// The decided branches, as jumps, and whatever the arms they stopped taking leave unreached.
void Optimizer::decideBranches() {
    if(!s.decisions.size()) return;

    auto& orphans = s.work;
    orphans.clear();

    for(auto& decision: s.decisions) {
        auto& branch = (JsInstBranch&)*decision.branch;
        auto block = base[branch.block];

        auto at = indexIn(block, &branch);
        if(at == block->instructionList.size()) continue;

        auto& taken = decision.answer ? branch.whenTrue : branch.whenFalse;
        auto& dropped = decision.answer ? branch.whenFalse : branch.whenTrue;

        release(branch);

        auto jump = new (target.arena) JsInstJump(branch.source);
        jump->id = branch.id;
        jump->block = branch.block;
        jump->to = taken;
        body.insts.set(base, branch.id.index, (JsInst*)jump - base);
        block->instructionList.set(base, at, (JsInst*)jump - base);

        for(auto supplied: jump->to.arguments.contents(base)) {
            if(!supplied) continue;
            base[supplied]->uses++;
            if(base[supplied]->id.index < liveUses.size()) liveUses[base[supplied]->id.index]++;
        }

        unlinkEdge(block, base[dropped.target], orphans);
        target.optimizer.decided++;
    }

    // The arms nothing reaches any more, and what only they reached.
    while(orphans.size()) {
        auto dead = body.block(base, JsBlockId(orphans.pop().unwrap()));
        if(dead->deleted || dead->predecessorCount()) continue;

        for(auto pointer: dead->instructions(base)) {
            auto& inst = *base[pointer];
            release(inst);
            forEachEdge(base, inst, [&](JsEdge& edge) {
                if(edge.target) unlinkEdge(dead, base[edge.target], orphans);
            });
        }

        dead->deleted = true;
    }

    /*
     * And every block the entry no longer reaches, which is not the same set: a loop inside a
     * dropped arm keeps a predecessor - its own back edge - so the walk above never orphans it,
     * and it went on to be emitted as a `var` for a block nothing enters, with the join after
     * it keeping a phantom predecessor (J91). The walk above is still what releases the common
     * case cheaply; this is the one reachability pass that makes it exact.
     */
    auto count = U32(body.blocks.size());
    auto& reached = s.reached;
    reached.clear();
    for(U32 i = 0; i < count; i++) reached.push(0);

    orphans.clear();
    orphans.push(base[body.entry]->index.index);
    reached[base[body.entry]->index.index] = 1;
    while(orphans.size()) {
        auto block = body.block(base, JsBlockId(orphans.pop().unwrap()));
        for(auto pointer: block->instructions(base)) {
            forEachEdge(base, *base[pointer], [&](JsEdge& edge) {
                if(!edge.target) return;
                auto next = base[edge.target]->index.index;
                if(reached[next]) return;
                reached[next] = 1;
                orphans.push(next);
            });
        }
    }

    for(U32 i = 0; i < count; i++) {
        auto dead = body.block(base, JsBlockId(i));
        if(dead->deleted || reached[i]) continue;

        for(auto pointer: dead->instructions(base)) {
            auto& inst = *base[pointer];
            release(inst);
            forEachEdge(base, inst, [&](JsEdge& edge) {
                if(!edge.target) return;
                auto to = base[edge.target];
                for(Size k = 0; k < to->incomingList.size(); k++) {
                    if(base[to->incomingList.get(base, k)] != dead) continue;
                    to->incomingList.remove(base, k);
                    break;
                }
            });
        }

        dead->deleted = true;
    }

    s.decisions.clear();
    target.cfg.ready = false;
    flow = nullptr;
}

void Optimizer::unlinkEdge(JsBlock* from, JsBlock* to, Array<U32>& orphans) {
    for(Size i = 0; i < to->incomingList.size(); i++) {
        if(base[to->incomingList.get(base, i)] != from) continue;
        to->incomingList.remove(base, i);
        break;
    }

    if(!to->incomingList.size() && to != base[body.entry]) orphans.push(to->index.index);
}

/*
 * `hostFixedCapacity` answered in JSIR, and the branches it and any other constant decide.
 *
 * The answer is a property of the operand's representation - a typed row has a capacity, a
 * plain one and anything else does not - and `codegen/js/structure.cpp` already spells it as a
 * constant with exactly this rule. Doing it there only was too late for one thing: the guarded
 * `growJsArray(box, "$v", n)` call was still in the body when the optimizer ran, and it is the
 * one use that kept an array local boxed (J86 §3). A condition that is a constant, or `eq`/`ne`
 * of two integral constants, then decides its branch through `decideBranches`.
 *
 * Only equality is folded: an order comparison of two constants depends on a signedness this
 * tier does not carry on the node, and nothing here needed one.
 */
bool Optimizer::constantNumber(JsValue* value, F64& out, U32 depth) {
    if(!value || !value->definer || depth > 4) return false;

    if(auto constant = constantOf(value)) {
        if(constant->kind != JsConstant::Number && constant->kind != JsConstant::Bool) return false;
        if(constant->kind == JsConstant::Number && !constant->integral) return false;
        out = constant->number;
        return true;
    }

    auto& inst = *base[value->definer];

    if(inst.kind == JsInst::Alias) return constantNumber(base[((JsInstAlias&)inst).from], out, depth + 1);

    if(inst.kind == JsInst::Binary) {
        auto& binary = (JsInstBinary&)inst;
        if(binary.op != mir::BinaryOp::Eq && binary.op != mir::BinaryOp::Ne) return false;

        F64 lhs = 0, rhs = 0;
        if(!constantNumber(base[binary.lhs], lhs, depth + 1)) return false;
        if(!constantNumber(base[binary.rhs], rhs, depth + 1)) return false;
        out = ((lhs == rhs) == (binary.op == mir::BinaryOp::Eq)) ? 1 : 0;
        return true;
    }

    return false;
}

void Optimizer::foldConstantBranches() {
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, JsBlockId(i));
        if(block->deleted) continue;

        for(Size at = 0; at < block->instructionList.size(); at++) {
            auto inst = base[block->instructionList.get(base, at)];
            if(inst->kind != JsInst::Host || !inst->result) continue;

            auto& host = (JsInstHost&)*inst;
            if(host.op != mir::HostOp::FixedCapacity || !host.arguments.size()) continue;

            JsConstant constant;
            constant.kind = JsConstant::Number;
            constant.number = target.rep(base[host.arguments.get(base, 0)]->rep).hasFixedCapacity() ? 1 : 0;

            auto folded = makeInst<JsInstConst>(block, JsRepId(), host.source, constant);
            replaceInst(inst, folded);
        }
    }

    s.decisions.clear();
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, JsBlockId(i));
        if(block->deleted || !block->instructionList.size()) continue;

        auto last = base[block->instructionList.get(base, block->instructionList.size() - 1)];
        if(last->kind != JsInst::Branch) continue;

        F64 answer = 0;
        if(!constantNumber(base[((JsInstBranch&)*last).condition], answer)) continue;
        s.decisions.push(JsOptDecision { last, answer != 0 });
    }

    decideBranches();
}

} // namespace jsir
