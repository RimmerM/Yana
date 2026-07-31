#include "opt_pass.h"

/*
 * Read/write combining over places.
 *
 * Three rewrites, all of them straight-line within one block:
 *
 *  - a load of a place something just wrote becomes the value it wrote;
 *  - a second load of a place nothing has written since becomes the first load;
 *  - a store of exactly the value a place is already known to hold goes away;
 *  - a store nothing reads before the same place is written again goes away.
 *
 * The last two are the ones the read-modify-write chains need. A packed field's write is `read the word,
 * replace the field's bits, write it back`, and a record built out of literals is that chain once per
 * field over a word that started at a constant - so forwarding the read makes the arithmetic
 * foldable, folding it makes the write-back a store of what is already there, and the third rule is
 * what actually removes it. None of them is a peephole on its own.
 *
 * ## Why this is a place pass rather than a memory pass
 *
 * Because at this altitude the answer to "can these two accesses touch the same storage" is
 * structural. A `Place` is a local plus a path of projections, `x.a` and `x.b` are different fields
 * of the same local by construction, and the borrow checker has already proved that two live `&`s of
 * overlapping paths do not coexist. Below the fork the same question is pointer disambiguation over
 * `add ptr, imm`, and on the JS tree it is reasoning about property names on values anything may
 * alias. This is the one stage where the ownership model pays for itself as an optimization.
 *
 * Two consequences worth stating, because they are why the packing expansion has to run *above* this
 * rather than below it:
 *
 *  - co-packed fields do not alias here. `h.version` and `h.length` share a machine word and are two
 *    different projections, so a write of one does not kill a read of the other - which is exactly
 *    Design.md's rule that the two are independent, restated as an optimization. Below the fork they
 *    are the same word and the information is gone.
 *  - a *whole-record* store still kills every field of it, because a shorter path is a prefix of the
 *    longer one and `mayAlias` says so.
 *
 * ## What is deliberately not done
 *
 * Nothing crosses a block boundary. That needs an availability dataflow and a way to place the
 * result, and the chains this exists for are straight-line. Nothing crosses anything that may write
 * storage this pass cannot see either - see `clobbers`, whose default is to forget everything.
 */

namespace {

// What one place is known to hold. A store and a load establish the same fact and are not kept
// apart: "this place holds %v" is what both a later load and a later store of %v rest on.
struct Known {
    Place place;
    ModulePtr<Value> value = nullptr;

    /*
     * The store that put it there, while nothing has read it back.
     *
     * Null once something has, and null for a fact a *load* established - that is storage which
     * already held the value, and there is no write to remove. Overwriting a place whose entry still
     * names a store is what makes that store dead: nothing between the two could have seen it, or
     * this would have been cleared.
     */
    ModulePtr<Inst> pending = nullptr;
};

struct Forwarder {
    OptContext& opt;
    Array<Known> known;

    // Per local, whether a callee could reach its storage - see `computeContainment`. Indexed by
    // local, and empty until one function has been walked.
    Array<U8> contained;

    /*
     * Whether two projection paths may reach the same storage.
     *
     * A prefix relation counts as overlap in both directions - writing `x` kills `x.a`, and writing
     * `x.a` kills a read of `x` - which is what makes the walk stop at the first step that proves
     * separation rather than needing the paths to be the same length.
     */
    bool pathsMayOverlap(Place& a, Place& b) {
        auto count = min(a.projections.size(), b.projections.size());

        for(Size i = 0; i < count; i++) {
            auto left = a.projections.get(opt.local, i);
            auto right = b.projections.get(opt.local, i);

            if(left.kind != right.kind) return true;

            switch(left.kind) {
                case ProjectionKind::Field:
                case ProjectionKind::Property:
                    // Two different fields of one aggregate are two different pieces of storage.
                    // That is true here even where the target co-packs them into one word, which is
                    // the whole reason this pass runs before the packing is expanded.
                    if(left.index != right.index) return false;
                    break;
                case ProjectionKind::Discriminant:
                    break;
                case ProjectionKind::Unit:
                    /*
                     * The word two co-packed fields became. Reached only where both paths agreed on
                     * the field in front of it, which is the whole reason opt_pack.cpp canonicalizes
                     * that index: `h.version` and `h.length` are two fields and one word, and it is
                     * the *field* indices above that the rule below is entitled to separate.
                     *
                     * Two different unit widths at one position would be two readings of one place,
                     * which nothing produces - so this compares nothing and lets the walk continue.
                     */
                    break;
                case ProjectionKind::Downcast:
                    /*
                     * Two constructors of one sum overlap: their payloads share the storage, and on
                     * both targets deliberately so. Only one is live at a time, but "which one" is a
                     * fact about the discriminant rather than about the path, so this declines to
                     * separate them.
                     */
                    if(left.index != right.index) return true;
                    break;
                case ProjectionKind::Index: {
                    // Two constant indices are two elements; anything else may be the same one.
                    auto leftIndex = constantValueOf(opt, left.value);
                    auto rightIndex = constantValueOf(opt, right.value);
                    if(!leftIndex || !rightIndex) return true;
                    if(leftIndex.unwrap() != rightIndex.unwrap()) return false;
                    break;
                }
                case ProjectionKind::Deref:
                    // Through a pointer, and this pass knows nothing about what it points at.
                    return true;
            }
        }

        return true;
    }

    bool mayAlias(Place& a, Place& b) {
        // A raw pointer may name anything at all, and a borrow may name anything the borrow checker
        // let it be taken of - which is a question about provenance rather than about the place, and
        // one this pass does not ask.
        if(a.root == PlaceRoot::Pointer || b.root == PlaceRoot::Pointer) return true;
        if(a.root == PlaceRoot::Borrow || b.root == PlaceRoot::Borrow) return true;

        if(a.root != b.root) return false;
        if(a.root == PlaceRoot::Local && a.local != b.local) return false;
        if(a.root == PlaceRoot::Global && a.global != b.global) return false;

        return pathsMayOverlap(a, b);
    }

    // The same storage rather than possibly the same: what a forward needs, as against what a kill
    // needs. Equal roots and equal paths, with an index projection equal only where both are the
    // same constant.
    bool sameStorage(Place& a, Place& b) {
        if(a.root != b.root) return false;
        if(a.root == PlaceRoot::Local && a.local != b.local) return false;
        if(a.root == PlaceRoot::Global && a.global != b.global) return false;
        if(a.root == PlaceRoot::Pointer || a.root == PlaceRoot::Borrow) {
            if(a.pointer != b.pointer) return false;
        }

        if(a.projections.size() != b.projections.size()) return false;

        for(Size i = 0; i < a.projections.size(); i++) {
            auto left = a.projections.get(opt.local, i);
            auto right = b.projections.get(opt.local, i);

            if(left.kind != right.kind || left.index != right.index) return false;
            if(!left.value && !right.value) continue;

            if(left.value == right.value) continue;

            auto leftIndex = constantValueOf(opt, left.value);
            auto rightIndex = constantValueOf(opt, right.value);
            if(!leftIndex || !rightIndex || leftIndex.unwrap() != rightIndex.unwrap()) return false;
        }

        return true;
    }

    void forget() { known.clear(); }

    // Everything an instruction this pass cannot see through may have written. Not everything: a
    // local whose address was never handed out is storage no callee has a way to name, and keeping
    // it is what lets a record built out of parameters survive the call in front of the read of it.
    void forgetExposed() {
        for(Size i = known.size(); i-- > 0;) {
            if(!staysInFrame(opt, contained, known[i].place)) known.remove(i);
        }
    }

    void forgetAliasing(Place& place) {
        for(Size i = known.size(); i-- > 0;) {
            if(mayAlias(known[i].place, place)) known.remove(i);
        }
    }

    /*
     * Whether an instruction may write storage this pass is tracking, or hand out a way to.
     *
     * The default is yes. An instruction kind this does not recognize is one whose effect on storage
     * has not been checked, and forgetting too much costs an optimization while forgetting too
     * little is a miscompile - which is the same asymmetry `borrowStaysHere` in codegen/js/gen.cpp
     * resolves the same way.
     */
    bool clobbers(Value& instruction) {
        switch(instruction.kind) {
            // Pure computation, and storage that has just come into existence with nothing in it.
            case Value::Alloc:
                return false;

            // Reads. `Copy` reads its place and writes storage of its own that nothing else names
            // yet, and `Borrow` of an immutable place cannot be written through.
            case Value::LoadPlace:
            case Value::Copy:
                return false;

            case Value::Borrow:
                return ((InstBorrow&)instruction).mut;

            // Handled by the caller, which knows which place they name.
            case Value::Init:
            case Value::Assign:
                return false;

            default:
                return !isPureValue(instruction);
        }
    }

    // Whether a value of this type is one whose *contents* a load answers with, rather than storage
    // the load merely names. Forwarding one of the second kind would replace a place with a value
    // that is not the same thing.
    bool forwardable(TypePtr type) {
        return type && !isUnit(opt.global, type) && !isMemoryType(opt.global, type);
    }

    ModulePtr<Value> knownValue(Place& place) {
        for(Size i = known.size(); i-- > 0;) {
            if(sameStorage(known[i].place, place)) return known[i].value;
        }

        return nullptr;
    }

    // One entry per piece of storage, so that "what is known about this place" and "which store put
    // it there" are one answer rather than the most recent of several.
    void remember(Place& place, ModulePtr<Value> value, ModulePtr<Inst> pending) {
        for(Size i = known.size(); i-- > 0;) {
            if(sameStorage(known[i].place, place)) known.remove(i);
        }

        known.push(Known { place, value, pending });
    }

    // Everything this access may have seen, which is no longer a write nobody read. Reads are marked
    // rather than forgotten: the *value* is still known, and it is only the store that stops being
    // removable.
    void markRead(Place& place) {
        for(auto& entry: known) {
            if(mayAlias(entry.place, place)) entry.pending = nullptr;
        }
    }

    void markRead(Value& instruction) {
        eachPlace(instruction, [&](const Place& place) { markRead(const_cast<Place&>(place)); });
    }

    /*
     * The store that is about to be overwritten, if nothing has read it.
     *
     * Within one block and one place, which is what makes it sound without an availability analysis:
     * the entry survives only while nothing aliasing it was read and nothing this pass cannot see
     * ran, and the overwrite is total because `sameStorage` compares the whole path - including a
     * unit projection's width, so half a word is never mistaken for all of it.
     */
    bool eliminateOverwritten(Place& place) {
        for(Size i = known.size(); i-- > 0;) {
            if(!sameStorage(known[i].place, place) || !known[i].pending) continue;

            auto pending = known[i].pending;
            known[i].pending = nullptr;
            eraseInstruction(opt, pending);
            return true;
        }

        return false;
    }

    void run(Block& block) {
        forget();

        for(Size i = 0; i < block.instructions.size(); i++) {
            auto pointer = block.instructions.get(opt.local, i);
            auto instruction = opt.local[pointer];

            switch(instruction->kind) {
                case Value::LoadPlace: {
                    auto& load = (InstLoadPlace&)*instruction;

                    // A load that is answered from a value already in hand is not a read of
                    // anything: it stops being an access at all, so the store it would have read
                    // stays removable. That is the case every read-modify-write chain is made of.
                    if(forwardable(load.type)) {
                        if(auto value = knownValue(load.place)) {
                            replaceValue(opt, (ModulePtr<Value>)pointer, value);
                            break;
                        }
                    }

                    markRead(load.place);
                    if(forwardable(load.type)) remember(load.place, (ModulePtr<Value>)pointer, nullptr);
                    break;
                }
                case Value::Init:
                case Value::Assign: {
                    auto& store = (InstInit&)*instruction;
                    auto type = opt.local[store.value]->type;

                    if(forwardable(type) && instruction->uses.isEmpty()) {
                        /*
                         * A write of what is already there.
                         *
                         * Sound because an assignment is only a write by the time this runs -
                         * whatever the old value's teardown owed has already been emitted as its own
                         * `InstDrop` by the drop pass, so there is nothing in an `Assign` but the
                         * store (see the Init/Assign case in resolve/lower.cpp). And the value's own
                         * ownership is untouched, because the place is being left holding the value
                         * it was already holding.
                         */
                        if(knownValue(store.place) == store.value) {
                            eraseInstruction(opt, pointer);
                            i--;
                            break;
                        }
                    }

                    // The store it replaces came out of the block in front of this one, so the walk
                    // has to step back over the gap it left.
                    if(forwardable(type) && eliminateOverwritten(store.place)) i--;

                    forgetAliasing(store.place);
                    if(forwardable(type)) remember(store.place, store.value, pointer);
                    break;
                }
                default:
                    if(clobbers(*instruction)) {
                        forgetExposed();

                        /*
                         * And the places the instruction names itself, which `forgetExposed` is
                         * exactly the wrong rule for: a `Move` out of a contained local, or a
                         * `Swap` of two of them, writes storage a callee could not have reached and
                         * this pass still has to forget. Conservative on a read - a place slot here
                         * is not known to be written - and it costs nothing, since an instruction
                         * that got this far is one the pass declined to model anyway.
                         */
                        eachPlace(*instruction, [&](const Place& place) {
                            forgetAliasing(const_cast<Place&>(place));
                        });
                    } else {
                        markRead(*instruction);
                    }
                    break;
            }
        }
    }
};

}

void forwardPlaces(OptContext& opt) {
    Forwarder forwarder { opt };
    computeContainment(opt, forwarder.contained);

    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        forwarder.run(*opt.local[blockPointer]);
    }
}
