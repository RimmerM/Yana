#include "opt_pass.h"

/*
 * A borrow that nothing needs as a reference, spliced out of the places that read through it.
 *
 * `&mut x` handed to a call is a real thing on both targets - an address natively, a box on JS. A
 * borrow whose *only* uses are reads and writes through it is not: every one of those accesses names
 * storage that is already named by the place the borrow was taken of, so the borrow denotes nothing
 * the accesses cannot say for themselves.
 *
 *      %b = borrow_mut %total          (nothing)
 *      %v = load [%b]            ->    %v = load %total
 *      assign [%b], %w                 assign %total, %w
 *
 * The rewrite is a substitution and nothing more: a place rooted in the borrow becomes the borrowed
 * place with the reader's own path appended. No instruction moves, so the order of every access is
 * exactly what it was, and the storage each one names is exactly what it named. That is the whole
 * soundness argument, and it is worth noticing that it does *not* rest on exclusivity - the borrow
 * checker's guarantee is what makes the borrow legal, not what makes this rewrite correct.
 *
 * ## Why this is worth a pass
 *
 * Because before inlining there were hardly any such borrows, and after it there are everywhere. A
 * mutator taking `&x` is the shape inlining is best at, and what it leaves behind is the caller's own
 * borrow with the callee's reads and writes now hanging off it - which is to say, exactly this.
 *
 * What it unlocks is different on each target and larger than removing an instruction:
 *
 *  - on JS a local that is mutably borrowed is *boxed*, because the host has no addresses and a
 *    callee writing through a reference needs an object to write into. Take the last borrow away and
 *    the local is an ordinary `var`, which forwarding then folds like any other;
 *  - natively the borrow is an address, and taking a local's address is what stops
 *    `promoteStackSlots` from holding it in a register.
 *
 * So this pass removes almost nothing by itself and is what lets three other passes see a local they
 * were previously told to leave alone. `Borrow.yana` is the fixture that says so.
 *
 * ## What is declined
 *
 * A borrow used as anything but a place root stays, which is the case it exists for: an argument, a
 * returned reference, a value stored somewhere. `mapOperands` counts a borrow-rooted place's root as
 * an operand, so "used only as a place root" is asked by counting both ways and comparing - a use the
 * place walk does not see is a use that needs the reference itself.
 *
 * The ownership instructions are declined as readers too. A `Drop`, `Move`, `Swap` or `Exchange`
 * naming a place rooted in the borrow is a decision the analyses already took against that place, and
 * rewriting the root underneath one is not something this pass can show is harmless - it is the same
 * refusal opt_inline.cpp makes about copying one, for the same reason.
 */

namespace {

// The readers this pass is willing to rewrite. Everything else either needs the reference itself or
// is an ownership decision that names a place for reasons of its own.
bool rewritableReader(const Value& instruction) {
    switch(instruction.kind) {
        case Value::LoadPlace:
        case Value::Init:
        case Value::Assign:
        case Value::Copy:
        case Value::Borrow:
            return true;
        default:
            return false;
    }
}

Size operandUses(OptContext& opt, Value& user, ModulePtr<Value> value) {
    Size count = 0;
    eachOperand(opt.local, user, [&](ModulePtr<Value> operand) { if(operand == value) count++; });
    return count;
}

Size placeRootUses(OptContext& opt, Value& user, ModulePtr<Value> value) {
    Size count = 0;
    eachPlace(user, [&](const Place& place) {
        if(place.root == PlaceRoot::Borrow && place.pointer == value) count++;
    });
    return count;
}

/*
 * Whether every reader of this borrow reaches it only through a place.
 *
 * The counts have to agree rather than the operand count being zero, because a borrow-rooted place
 * *is* an operand as far as `mapOperands` is concerned - that is how the root gets rewritten when a
 * value is replaced. What the comparison asks is whether any use is something the place walk did not
 * account for, which is exactly a use of the reference as a value.
 */
bool collapsible(OptContext& opt, ModulePtr<Inst> pointer) {
    auto borrow = opt.local[pointer];
    if(borrow->useCount() == 0) return false;

    for(auto user: borrow->uses(opt.local)) {
        auto& instruction = *opt.local[user];
        if(!rewritableReader(instruction)) return false;

        auto asPlace = placeRootUses(opt, instruction, (ModulePtr<Value>)pointer);
        if(!asPlace) return false;
        if(operandUses(opt, instruction, (ModulePtr<Value>)pointer) != asPlace) return false;
    }

    return true;
}

// The borrowed place with a reader's own path appended. A fresh projection list rather than the
// borrowed place's, since several readers are built from one base and a shared list would have every
// reader's path appended to the same one.
Place substituted(OptContext& opt, const Place& base, Place& reader) {
    Place result = base;
    result.projections = {};

    auto& borrowed = const_cast<Place&>(base).projections;
    for(Size i = 0; i < borrowed.size(); i++) {
        result.projections.push(opt.program.arena, borrowed.get(opt.local, i));
    }

    for(Size i = 0; i < reader.projections.size(); i++) {
        result.projections.push(opt.program.arena, reader.projections.get(opt.local, i));
    }

    return result;
}

/*
 * Every reader repointed, and its use bookkeeping restated around the substitution.
 *
 * What changes is which storage a place is rooted in, and a root is a use recorded against the value
 * the storage came from - so a reader that named the borrow now names whatever the borrowed place is
 * rooted in, plus one use per index in the path it inherited. `rewriteOperands` states both halves
 * around the substitution, which is the exact repair; working out which entries moved is the version
 * of this that is wrong in one case nobody thought of.
 *
 * Over a snapshot of the use list, because that list is what the repair is editing.
 */
void rewriteReaders(OptContext& opt, ModulePtr<Inst> pointer, const Place& borrowed) {
    SmallArray<ModulePtr<Inst>, 16> readers;
    for(auto user: opt.local[pointer]->uses(opt.local)) readers.push(user);

    for(auto user: readers) {
        opt.ir().rewriteOperands(user, [&](Value& instruction) {
            Place* places[kMaxPlaces];
            auto count = instructionPlaceSlots(instruction, places);

            for(Size i = 0; i < count; i++) {
                if(places[i]->root != PlaceRoot::Borrow) continue;
                if(places[i]->pointer != (ModulePtr<Value>)pointer) continue;

                *places[i] = substituted(opt, borrowed, *places[i]);
            }
        });
    }
}

}

void collapseBorrows(OptContext& opt) {
    SmallArray<ModulePtr<Inst>, 16> collapsed;

    /*
     * In instruction order, which is what makes a chain of borrows collapse in one pass: an outer
     * borrow is defined before the borrow taken through it, so by the time the inner one is looked at
     * its own place has already been rewritten to name the outer one's storage.
     *
     * The lists stay right as this goes, which is what lets `collapsible` be asked about the inner
     * borrow after the outer one has been rewritten underneath it.
     */
    for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(auto pointer: block->instructions(opt.local)) {
            if(opt.local[pointer]->kind != Value::Borrow) continue;
            if(!collapsible(opt, pointer)) continue;

            rewriteReaders(opt, pointer, ((InstBorrow&)*opt.local[pointer]).place);
            collapsed.push(pointer);
        }
    }

    if(collapsed.isEmpty()) return;

    // Nothing reads any of them by now - that is what the rewrite above did - which is the condition
    // `eraseInstruction` asserts rather than assumes.
    for(auto pointer: collapsed) opt.ir().eraseInstruction(pointer);
    opt.changed = true;
}
