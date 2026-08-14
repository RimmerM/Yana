#include "analyze_pass.h"

/*
 * Owner mutation demand (Design.md's "Binding mutability and owner mutation demand").
 *
 * Deliberately keyed on the root rather than on the binding that named it, and deliberately not
 * raised by initialization: filling storage that held nothing is what every owned value's first
 * instruction does, so counting it would make every root writable and the analysis would answer
 * the same thing everywhere. Overwriting a live value is the operation that needs writable storage,
 * which is the whole reason Init and Assign are two instructions.
 */
static void raiseDemand(Analysis& analysis, const Provenance& roots, const ReprRequirements& what) {
    // Over the roots rather than over the frame - see IndexSet::forEach.
    roots.locals.forEach([&](Size l) { analysis.demand[l].raise(what); });
}

void computeDemand(Analysis& analysis) {
    for(Size l = 0; l < analysis.localCount; l++) analysis.demand.push(ReprRequirements());

    auto writable = ReprRequirements { MutationDemand::Writable, false, false };
    auto unknown = ReprRequirements { MutationDemand::Unknown, false, false };

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];

        switch(instruction.kind) {
            case Value::Assign: {
                auto& write = (InstInit&)instruction;
                ScratchProvenance roots(analysis);
                placeProvenance(analysis, write.place, *roots);
                raiseDemand(analysis, *roots, writable);

                /*
                 * Replacing indirect storage this owner holds is what a regrow is.
                 *
                 * There is no `resize` operation in the language to key this on, so the structural
                 * definition is the one above: an assignment - not an initialization - of a pointer
                 * into a projection of a root replaced storage that root was holding. That is
                 * exactly what an array's grow does and what nothing else does, and it is the fact
                 * that keeps a growable array's buffer off the frame.
                 */
                if(write.place.projections.isNotEmpty() &&
                   isPointer(analysis.global, analysis.local[write.value]->type)) {
                    raiseDemand(analysis, *roots, ReprRequirements { MutationDemand::ReadOnly, false, true });
                }

                break;
            }

            case Value::Borrow:
                if(((InstBorrow&)instruction).mut) {
                    ScratchProvenance roots(analysis);
                    placeProvenance(analysis, ((InstBorrow&)instruction).place, *roots);
                    raiseDemand(analysis, *roots, writable);
                }

                break;

            case Value::Address: {
                // Design.md's Pointers section: the memory a raw pointer names is always mutable,
                // so handing one out is both a write capability and a demand for storage to exist.
                ScratchProvenance addressed(analysis);
                placeProvenance(analysis, ((InstAddress&)instruction).place, *addressed);
                raiseDemand(analysis, *addressed,
                            ReprRequirements { MutationDemand::Writable, true, false });
                break;
            }

            case Value::Call: {
                auto& call = (InstCall&)instruction;
                auto summary = summaryOf(analysis, call.callee);
                U16 index = 0;

                for(auto arg: call.args.contents(analysis.local)) {
                    auto& roots = provenanceOf(analysis, arg);

                    if(!summary || index >= summary->args.size()) {
                        if(refersToStorage(analysis, analysis.local[arg]->type)) {
                            raiseDemand(analysis, roots, unknown);
                        }
                    } else {
                        raiseDemand(analysis, roots, summary->args.get(analysis.local, index).requirements);
                    }

                    index++;
                }

                break;
            }

            case Value::CallDyn:
                /*
                 * Deliberately not read off the signature, unlike the escape and return-root rules
                 * next door. Those are contracts a function *type* states, and this is not one: the
                 * demand is what the callee's body turned out to need of the caller's storage, and a
                 * convention says nothing about it - a borrow argument is still passed as an address
                 * into a body this call site cannot see. `unknown` is the top of the lattice and
                 * selects the conservative representation, which is the right answer here.
                 */
                for(auto arg: ((InstCallDyn&)instruction).args.contents(analysis.local)) {
                    if(refersToStorage(analysis, analysis.local[arg]->type)) {
                        raiseDemand(analysis, provenanceOf(analysis, arg), unknown);
                    }
                }

                break;

            case Value::GenCall:
                for(auto arg: ((InstGenCall&)instruction).args.contents(analysis.local)) {
                    if(refersToStorage(analysis, analysis.local[arg]->type)) {
                        raiseDemand(analysis, provenanceOf(analysis, arg), unknown);
                    }
                }

                break;

            case Value::Native:
                // Native's block operations write through whatever they were given, and there is
                // no signature here to say which of the two arguments that was.
                for(auto arg: ((InstNative&)instruction).args.contents(analysis.local)) {
                    if(refersToStorage(analysis, analysis.local[arg]->type)) {
                        raiseDemand(analysis, provenanceOf(analysis, arg),
                                    ReprRequirements { MutationDemand::Writable, true, false });
                    }
                }

                break;

            default:
                break;
        }
    }
}
