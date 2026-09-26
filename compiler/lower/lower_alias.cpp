/*
 * The rewrite itself. See lower_alias.h for what it proves and why it is here.
 */

#include "lower_alias.h"
#include "lower_builder.h"
#include "lower_store.h"

namespace {

// What a pointer's holder may do with it: write through it, or keep it past the call - stored,
// returned, or handed somewhere this cannot follow.
constexpr U8 kWrites = 1;
constexpr U8 kKeeps = 2;
constexpr U8 kAnything = kWrites | kKeeps;

/*
 * The effects of a callee's parameters, read off the lowered bodies.
 *
 * The language states them - a `shared` parameter is read-only and a call-bounded borrow is not
 * kept - but in the declarations, which this tier does not have; the bodies say the same where the
 * callee is direct, and the walk is only asked about a candidate's callees, which is a handful per
 * function. A cycle through a parameter still being walked answers `kAnything` rather than the
 * optimistic nothing, since an answer memoized under an optimistic guess is not revised when the
 * guess turns out wrong. A budget stops the walk from following a large call graph.
 */
struct Effects {
    LowerBase base;
    HashMap<U64, U8> memo;
    U32 budget = 4096;

    U8 ofParam(LowerFunction* fun, Size index, U32 depth) {
        if(depth > 12 || index >= fun->args.size() || fun->blocks.size() == 0) return kAnything;

        auto key = (U64((fun - base).offset) << 16) | U64(index);
        if(auto known = memo.getValue(key)) return known.unwrap();

        // In progress: a cycle back here answers the cautious way - see above.
        *memo.add(key).value = kAnything;
        auto answer = ofValue(&base[fun->args.get(base, index)]->result, depth, 0);
        *memo.add(key).value = answer;
        return answer;
    }

    // What the holders of this pointer, and of everything reached from it by a constant offset or a
    // join, may do with it.
    U8 ofValue(LowerValue* value, U32 callDepth, U32 depth) {
        if(depth > 8 || budget == 0) return kAnything;
        budget--;

        U8 effects = 0;
        auto self = value - base;

        for(auto userPtr: value->uses.contents(base)) {
            auto user = base[userPtr];

            switch(user->kind) {
                case LowerInst::Load:
                case LowerInst::Cmp:
                    break;
                case LowerInst::Store: {
                    auto store = (LowerInstStore*)user;
                    if(store->value == self) effects |= kKeeps;
                    if(store->to == self) effects |= kWrites;
                    break;
                }
                case LowerInst::Copy: {
                    auto copy = (LowerInstCopy*)user;
                    if(copy->count == self) effects |= kKeeps;
                    if(copy->to == self) effects |= kWrites;
                    break;
                }
                case LowerInst::SetPattern: {
                    auto fill = (LowerInstSetPattern*)user;
                    if(fill->count == self || fill->pattern == self) effects |= kKeeps;
                    if(fill->to == self) effects |= kWrites;
                    break;
                }
                case LowerInst::Add: {
                    auto add = (LowerInstBinary*)user;
                    auto other = base[add->lhs == self ? add->rhs : add->lhs]->inst();
                    if(add->lhs == add->rhs || !other || other->kind != LowerInst::Imm) return kAnything;
                    effects |= ofValue(&add->result, callDepth, depth + 1);
                    break;
                }
                case LowerInst::Phi:
                case LowerInst::Select:
                    effects |= ofValue(((LowerInstSingle*)user)->created().ptr, callDepth, depth + 1);
                    break;
                case LowerInst::Call: {
                    auto callee = directCallee(base, user);
                    if(!callee || user->usedCount != callee->args.size() + 1) return kAnything;

                    auto used = user->used();
                    if(used.ptr[0] == self) return kAnything;
                    for(Size i = 1; i < used.length; i++) {
                        if(used.ptr[i] == self) effects |= ofParam(callee, i - 1, callDepth + 1);
                    }
                    break;
                }
                default:
                    return kAnything;
            }

            if(effects == kAnything) return effects;
        }

        return effects;
    }
};

Size indexIn(LowerBase base, LowerBlock* block, LowerInst* inst) {
    auto count = block->instructions.size();
    for(Size i = 0; i < count; i++) {
        if(base[block->instructions.get(base, i)] == inst) return i;
    }
    return count;
}

// One store of the copy: where in the local it lands, and the load of the source it writes.
struct Piece {
    LowerInstStore* store;
    LowerInstLoad* load;
    U64 offset;
    U32 width;
};

struct Candidate {
    LowerBase base;
    LowerModule& module;
    Effects& effects;

    LowerInstAlloca* local;
    LowerBlock* block;
    U64 bytes = 0;

    SmallArray<Piece, 16> pieces;
    SmallArray<LowerInst*, 16> readers;
    LowerInstPhi* phi = nullptr;
    SmallArray<LowerInst*, 16> phiReaders;

    // The offsets of the local itself - the `add`s its pieces and readers address it through, which
    // the rewrite points at the source and which therefore have to sit below the source's address.
    SmallArray<LowerInst*, 16> offsets;

    // Every user of `value`, which is the local moved by `offset`: a store into it is a piece, and
    // anything else has to be a read.
    bool collect(LowerValue* value, U64 offset, U32 depth, SmallArray<LowerInst*, 16>& into, bool allowPhi) {
        if(depth > 4) return false;
        auto self = value - base;

        for(auto userPtr: value->uses.contents(base)) {
            auto user = base[userPtr];

            switch(user->kind) {
                case LowerInst::Store: {
                    auto store = (LowerInstStore*)user;
                    if(store->value == self || !allowPhi) return false;

                    auto stored = base[store->value]->inst();
                    if(stored->kind != LowerInst::Load || stored->createdCount != 1) return false;
                    auto load = (LowerInstLoad*)stored;
                    if(load->isVolatile() || store->isVolatile() || load->getWidth() != store->getWidth()) return false;

                    pieces.push(Piece { store, load, offset, store->getWidth() });
                    break;
                }
                case LowerInst::Add: {
                    auto add = (LowerInstBinary*)user;
                    auto other = base[add->lhs == self ? add->rhs : add->lhs]->inst();
                    if(add->lhs == add->rhs || !other || other->kind != LowerInst::Imm) return false;
                    if(((LowerImm*)other)->i >= bytes) return false;
                    if(allowPhi) offsets.push(user);
                    if(!collect(&add->result, offset + ((LowerImm*)other)->i, depth + 1, into, allowPhi)) return false;
                    break;
                }
                case LowerInst::Load:
                case LowerInst::Cmp:
                    into.push(user);
                    break;
                case LowerInst::Call: {
                    auto callee = directCallee(base, user);
                    if(!callee || user->usedCount != callee->args.size() + 1) return false;

                    auto used = user->used();
                    if(used.ptr[0] == self) return false;
                    for(Size i = 1; i < used.length; i++) {
                        if(used.ptr[i] == self && effects.ofParam(callee, i - 1, 0) != 0) return false;
                    }

                    into.push(user);
                    break;
                }
                case LowerInst::Phi:
                    // The one join the local may cross, and only whole.
                    if(!allowPhi || offset != 0 || phi) return false;
                    phi = (LowerInstPhi*)user;
                    break;
                default:
                    return false;
            }
        }

        return true;
    }

    // Whether this instruction may write any of the source's bytes.
    bool mayWrite(LowerInst* inst, LowerValue* root, U64 from, U64 extent) {
        if(inst->kind == LowerInst::Store || inst->kind == LowerInst::Copy || inst->kind == LowerInst::SetPattern) {
            for(auto piece: pieces) {
                if((LowerInst*)piece.store == inst) return false;
            }

            LowerPtr<LowerValue> to;
            U64 width = kUnbounded;

            if(inst->kind == LowerInst::Store) {
                to = ((LowerInstStore*)inst)->to;
                width = ((LowerInstStore*)inst)->getWidth();
            } else if(inst->kind == LowerInst::Copy) {
                to = ((LowerInstCopy*)inst)->to;
            } else {
                to = ((LowerInstSetPattern*)inst)->to;
            }

            auto place = placeOf(base, to, width);
            if(!place.root) return true;
            if(place.root == root) {
                LowerPlace source;
                source.root = root;
                source.allocation = true;
                source.offset = from;
                source.bytes = extent;
                return mayOverlap(place, source);
            }

            // A pointer reached through a join may be one into the source; anything else cannot,
            // since nothing kept the source's address to load, return or pass it back.
            auto rootKind = place.root->inst()->kind;
            return rootKind == LowerInst::Phi || rootKind == LowerInst::Select;
        }

        if(inst->kind == LowerInst::Call) {
            auto callee = directCallee(base, inst);
            auto used = inst->used();

            for(Size i = 1; i < used.length; i++) {
                auto place = placeOf(base, used.ptr[i], 1);
                if(!place.root) return true;

                auto rootKind = place.root->inst()->kind;
                if(place.root != root && rootKind != LowerInst::Phi && rootKind != LowerInst::Select) continue;

                if(!callee || inst->usedCount != callee->args.size() + 1) return true;
                if(effects.ofParam(callee, i - 1, 0) & kWrites) return true;
            }

            return false;
        }

        return hasLowerTrait(inst, kLowerWrites | kLowerOrdered);
    }

    bool run() {
        if(!collect(&local->result, 0, 0, readers, true)) return false;
        if(pieces.size() == 0) return false;

        // The pieces tile the local exactly, and read one source at one displacement.
        for(Size i = 1; i < pieces.size(); i++) {
            for(Size j = i; j > 0 && pieces[j - 1].offset > pieces[j].offset; j--) {
                auto t = pieces[j]; pieces[j] = pieces[j - 1]; pieces[j - 1] = t;
            }
        }

        U64 next = 0;
        LowerValue* root = nullptr;
        U64 displacement = 0;
        Size first = block->instructions.size();
        Size last = 0;

        for(auto& piece: pieces) {
            if(piece.offset != next) return false;
            next += piece.width;

            if(base[piece.store->block] != block || base[piece.load->block] != block) return false;

            auto place = placeOf(base, piece.load->from, piece.width);
            if(!place.root || place.root->inst()->kind != LowerInst::Alloca) return false;
            if(place.offset < piece.offset) return false;

            if(!root) {
                root = place.root;
                displacement = place.offset - piece.offset;
            } else if(root != place.root || place.offset - piece.offset != displacement) {
                return false;
            }

            auto at = indexIn(base, block, piece.load);
            auto stored = indexIn(base, block, piece.store);
            if(at < first) first = at;
            if(stored > last) last = stored;
        }

        if(next != bytes || root == &local->result) return false;

        // Every direct reader below the last store, in this block.
        for(auto reader: readers) {
            if(base[reader->block] != block || indexIn(base, block, reader) <= last) return false;
        }

        LowerBlock* join = nullptr;
        Size end = 0;

        if(phi) {
            join = base[phi->block];
            if(join == block) return false;

            auto used = phi->used();
            auto sources = phi->sources();
            for(Size i = 0; i < used.length; i++) {
                if(base[used.ptr[i]] == &local->result && base[sources[i]] != block) return false;
            }

            if(!collect(&phi->result, 0, 0, phiReaders, false)) return false;
            for(auto reader: phiReaders) {
                if(base[reader->block] != join) return false;
                auto at = indexIn(base, join, reader);
                if(at == join->instructions.size()) return false;
                if(at + 1 > end) end = at + 1;
            }
        }

        // The source is an allocation of this frame whose address nobody keeps.
        if(effects.ofValue(root, 0, 0) & kKeeps) return false;

        for(Size i = first; i < block->instructions.size(); i++) {
            if(mayWrite(base[block->instructions.get(base, i)], root, displacement, bytes)) return false;
        }

        for(Size i = 0; join && i < end; i++) {
            if(mayWrite(base[join->instructions.get(base, i)], root, displacement, bytes)) return false;
        }

        // The rewrite: the piece at offset zero names the source's first byte.
        auto source = pieces[0].load->from;

        /*
         * And every offset of the local has to be computed below that address, since it is about to
         * read it. The pieces are in offset order rather than program order, so the offset-zero
         * piece's load need not be the first - and an `add %local, 8` the lowering or a CSE placed
         * above it would be left reading a value defined further down. An address defined above this
         * block dominates the whole of it; one defined in it has to come first.
         */
        auto sourceDef = base[source]->inst();
        auto sourceInBlock = sourceDef && base[sourceDef->block] == block;
        auto sourceAt = sourceInBlock ? indexIn(base, block, sourceDef) : 0;

        for(auto offset: offsets) {
            if(base[offset->block] != block) return false;
            if(sourceInBlock && indexIn(base, block, offset) <= sourceAt) return false;
        }

        for(auto& piece: pieces) {
            detach(base, piece.store);
            block->instructions.remove(base, indexIn(base, block, piece.store));
        }

        replaceUses(base, module.arena, &local->result - base, source);
        return true;
    }
};

} // namespace

void aliasReadOnlyCopies(LowerBase base, LowerModule& module, LowerFunction& fun) {
    SmallArray<LowerInstAlloca*, 16> locals;

    for(auto blockPtr: fun.blocks.contents(base)) {
        for(auto instPtr: base[blockPtr]->instructions.contents(base)) {
            auto inst = base[instPtr];
            if(inst->kind != LowerInst::Alloca) continue;

            auto count = base[((LowerInstAlloca*)inst)->byteCount]->inst();
            if(count->kind != LowerInst::Imm) continue;

            // Below two words promotion takes the copy apart; above a few lines it is not a record
            // a single reader walks.
            auto bytes = ((LowerImm*)count)->i;
            if(bytes > 16 && bytes <= 256) locals.push((LowerInstAlloca*)inst);
        }
    }

    if(locals.size() == 0) return;

    Effects effects { base };

    for(auto local: locals) {
        Candidate candidate { base, module, effects, local, base[local->block] };
        candidate.bytes = ((LowerImm*)base[local->byteCount]->inst())->i;
        candidate.run();
    }
}
