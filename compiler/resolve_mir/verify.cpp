#include "verify.h"

namespace mir {

namespace {

struct Verifier {
    Context& context;
    Program& program;
    Function& function;
    Body& body;
    MirBase base;
    DeclBase decl;

    U32 errors = 0;

    template<class... T>
    void report(LocationId source, StringView text, T&&... arguments) {
        errors++;
        context.diagnostics.error(text, source, forward<T>(arguments)...);
    }

    /*
     * What an operand names, as an occurrence.
     *
     * The one thing every structural check over an operand needs and the representation does not
     * hand over directly: a result's occurrence is on its node, a constant's is owned by
     * construction, and a place's is on the place record. Answers false when the operand does not
     * name anything, which `checkOperand` has already reported.
     */
    bool occurrenceOf(Operand operand, Occurrence& into) {
        switch(operand.source()) {
            case SourceKind::Result: {
                if(operand.id() >= body.nodes.size()) return false;

                auto producer = body.node(base, NodeId(operand.id()));
                if(!hasResult(producer->kind)) return false;

                into = valueOccurrence(*producer);
                return true;
            }
            case SourceKind::Place: {
                if(operand.id() >= body.places.size()) return false;
                into = body.place(base, PlaceId(operand.id()))->occurrence;
                return true;
            }
            case SourceKind::Constant: {
                if(operand.id() >= body.constants.size()) return false;
                into = owned(body.constant(base, ConstantId(operand.id())).type);
                return true;
            }
        }

        return false;
    }

    // What an operand may name, by category. The one question a structural verifier can settle about
    // an operand without knowing its role, and the one the current representation cannot ask at all:
    // an operand there is a value handle, and "is this the kind of thing that may stand here" has no
    // form.
    bool checkOperand(Node& node, Operand operand) {
        if(operand.mode() >= UseMode::Count) {
            report(node.source, "internal: %@ has an operand with no use mode"_v,
                   nodeMnemonic(node.kind));
            return false;
        }

        switch(operand.source()) {
            case SourceKind::Result: {
                if(operand.id() >= body.nodes.size()) {
                    report(node.source, "internal: %@ names result %@, which does not exist"_v,
                           nodeMnemonic(node.kind), operand.id());
                    return false;
                }

                auto producer = body.node(base, NodeId(operand.id()));
                if(!hasResult(producer->kind)) {
                    report(node.source, "internal: %@ names %@ as a value, and it produces none"_v,
                           nodeMnemonic(node.kind), nodeMnemonic(producer->kind));
                    return false;
                }

                // Ordering, not dominance: dominance needs the CFG and belongs to the SSA verifier,
                // but a use of a node created later than its user is a builder bug either way.
                if(operand.id() >= node.id.index) {
                    report(node.source, "internal: %@ names result %@, which it precedes"_v,
                           nodeMnemonic(node.kind), operand.id());
                    return false;
                }
                break;
            }
            case SourceKind::Place:
                if(operand.id() >= body.places.size()) {
                    report(node.source, "internal: %@ names place %@, which does not exist"_v,
                           nodeMnemonic(node.kind), operand.id());
                    return false;
                }
                break;
            case SourceKind::Constant:
                if(operand.id() >= body.constants.size()) {
                    report(node.source, "internal: %@ names constant %@, which does not exist"_v,
                           nodeMnemonic(node.kind), operand.id());
                    return false;
                }
                break;
        }

        return true;
    }

    /*
     * What the operand is *for*, checked against what it is - Analysis-IR.md §2.1.
     *
     * The half a walk over bare operands cannot do. Each of these is a statement the representation
     * makes about a node's shape and nothing enforced until now: a read names storage, a write's
     * destination is storage this function may write exclusively, a value position does not name
     * storage at all, and a transfer names something there was ownership to transfer.
     *
     * `verifyBody` runs over a sealed body, so a failure here is the *resolver's*: the builder
     * refuses what it can decide at construction, and this is the same rules restated where the
     * whole body is visible. Both, deliberately - a builder assertion fires in an assertion build
     * and this reports in every build.
     */
    void checkRole(Node& node, Operand operand, OperandRole role) {
        auto mnemonic = nodeMnemonic(node.kind);
        auto wantsPlace = role == OperandRole::ReadSource || role == OperandRole::WriteTarget;

        if(wantsPlace && operand.source() != SourceKind::Place) {
            report(node.source, "internal: the %@ of %@ is not a place"_v,
                   operandRoleName(role), mnemonic);
            return;
        }

        if(!wantsPlace && operand.source() == SourceKind::Place) {
            report(node.source, "internal: the %@ of %@ is a place where a value is wanted"_v,
                   operandRoleName(role), mnemonic);
            return;
        }

        Occurrence occurrence;
        if(!occurrenceOf(operand, occurrence)) return;

        if(!occurrence.type) {
            report(node.source, "internal: the %@ of %@ has no type"_v, operandRoleName(role),
                   mnemonic);
            return;
        }

        if(role == OperandRole::WriteTarget) {
            // Writing is exclusive access, and saying so with the same mode a `&` argument uses is
            // what lets one overlap check see both - see `NodeWrite`.
            if(operand.mode() != UseMode::BorrowUnique) {
                report(node.source, "internal: %@ takes its destination under %@ rather than %@"_v,
                       mnemonic, useModeName(operand.mode()),
                       useModeName(UseMode::BorrowUnique));
            }

            /*
             * Access, which is about the *storage*, and not `writable`, which is about the binding.
             *
             * A `let x` place is not writable and is still written exactly once, by the `Init` that
             * establishes it - so the binding's permission is a question about the write's
             * `Destination` and is checked in `checkNode`, where that field is in hand. What holds
             * for every write whatever its destination is this: storage the function only shares is
             * not storage it may write.
             */
            auto place = body.place(base, operand.placeId());
            if(place->occurrence.contract.access() == Access::Shared) {
                report(node.source,
                       "internal: %@ writes through a place this function only shares"_v, mnemonic);
            }
        }

        /*
         * The two mode rules a structural pass can settle.
         *
         * `CopyTrivial` claims evidence that a second owner costs nothing to make and nothing to
         * destroy, and a type with a teardown has neither. `Take` claims there was ownership to move
         * and leaves the source uninitialized, so a source nobody owns here - a borrowed place, a
         * constant that lives in the body's table - has nothing to give up. Both are how an
         * ownership bug reaches the backend as a double free rather than as a diagnostic.
         */
        if(operand.mode() == UseMode::CopyTrivial && !isTriviallyCopyable(decl, occurrence.type)) {
            report(node.source, "internal: %@ copies its %@ trivially, and its type is not"_v,
                   mnemonic, operandRoleName(role));
        }

        if(operand.mode() == UseMode::Take && operand.source() == SourceKind::Place) {
            auto place = body.place(base, operand.placeId());
            if(!place->occurrence.contract.isOwned()) {
                report(node.source, "internal: %@ takes from a place this function does not own"_v,
                       mnemonic);
            }
        }
    }

    void checkPlaces() {
        for(U32 i = 0; i < body.places.size(); i++) {
            auto place = body.place(base, PlaceId(i));

            if(place->id.index != i) {
                report(kNullLocation, "internal: place %@ is stored at index %@"_v,
                       place->id.index, i);
            }

            if(place->isRoot()) {
                if(place->parent) {
                    report(kNullLocation, "internal: root place %@ has a parent"_v,
                           placeKindName(place->kind));
                }

                if(place->kind == Place::Local && place->payload >= body.locals.size()) {
                    report(kNullLocation, "internal: place names local %@, which does not exist"_v,
                           place->payload);
                }
            } else if(!place->parent) {
                report(kNullLocation, "internal: projection place %@ has no parent"_v,
                       placeKindName(place->kind));
            } else {
                /*
                 * A projection of a place nothing may write is a place nothing may write, and a
                 * projection of a place the function only shares is shared - both are recorded on
                 * the record rather than walked to at every use, which means both are things that
                 * can be recorded *wrong*. This is where a walk that only happens once checks them.
                 */
                auto parent = base[place->parent];

                if(place->writable && !parent->writable) {
                    report(kNullLocation,
                           "internal: place %@ is writable and its parent is not"_v, i);
                }

                if(place->occurrence.contract.access() == Access::Unique &&
                   parent->occurrence.contract.access() == Access::Shared) {
                    report(kNullLocation,
                           "internal: place %@ claims unique access through a shared parent"_v, i);
                }
            }

            if(!place->occurrence.type) {
                report(kNullLocation, "internal: place %@ has no type"_v, i);
            }
        }
    }

    /*
     * The parameters, which are nodes and are not in the instruction list.
     *
     * Two things a structural pass can settle about one: that it is the kind it is stored as, and
     * that its recorded position matches where it is stored. The second is what an edge's argument
     * list is matched against, so a parameter whose index disagreed with its slot would make every
     * edge check compare the wrong pair.
     */
    void checkArguments(Block& block) {
        for(U32 i = 0; i < block.arguments.size(); i++) {
            auto node = base[block.arguments.get(base, i)];

            if(node->kind != Node::Argument) {
                report(node->source, "internal: block %@ holds %@ as a parameter"_v,
                       block.index.index, nodeMnemonic(node->kind));
                continue;
            }

            if(((NodeArgument*)node)->index != i) {
                report(node->source, "internal: parameter %@ of block %@ records position %@"_v,
                       i, block.index.index, ((NodeArgument*)node)->index);
            }

            if(!valueOccurrence(*node).type) {
                report(node->source, "internal: parameter %@ of block %@ has no type"_v,
                       i, block.index.index);
            }
        }
    }

    /*
     * One outgoing edge against the parameters it satisfies - §5.9.
     *
     * The arity was checked when the edge was built; what could not be checked then is the pair of
     * occurrences, because a successor's parameters and a predecessor's arguments are not both in
     * hand until the body is sealed. Both halves of the occurrence are compared and not just the
     * type: a merge that declares an owned parameter and an arm that hands over a borrow is exactly
     * the transfer the block-argument representation exists to make visible, and comparing types
     * alone would let it through.
     */
    void checkEdge(Node& node, Edge& edge) {
        if(!edge.target) {
            report(node.source, "internal: %@ has an edge with no target"_v,
                   nodeMnemonic(node.kind));
            return;
        }

        auto target = base[edge.target];
        if(edge.arguments.size() != target->arguments.size()) {
            report(node.source,
                   "internal: %@ supplies %@ arguments to block %@, which declares %@"_v,
                   nodeMnemonic(node.kind), edge.arguments.size(), target->index.index,
                   target->arguments.size());
            return;
        }

        for(U32 i = 0; i < edge.arguments.size(); i++) {
            auto operand = edge.arguments.get(base, i);
            if(!checkOperand(node, operand)) continue;

            checkRole(node, operand, OperandRole::EdgeArgument);

            Occurrence supplied;
            if(!occurrenceOf(operand, supplied)) continue;

            auto& declared = valueOccurrence(*base[target->arguments.get(base, i)]);

            if(!sameType(supplied.type, declared.type)) {
                report(node.source,
                       "internal: %@ supplies argument %@ of type %@ to block %@, which declares %@"_v,
                       nodeMnemonic(node.kind), i, typeText(decl, supplied.type),
                       target->index.index, typeText(decl, declared.type));
                continue;
            }

            if(supplied.contract != declared.contract || supplied.shape != declared.shape) {
                report(node.source,
                       "internal: %@ hands argument %@ to block %@ under a contract it does not declare"_v,
                       nodeMnemonic(node.kind), i, target->index.index);
            }
        }
    }

    // The rules that are about one node's own fields rather than about one operand.
    void checkNode(Node& node) {
        if(node.kind != Node::Write) return;

        auto& written = (NodeWrite&)node;

        /*
         * An assignment is not an initialization, which is the distinction this whole node exists
         * for - §5.5. A `let x` place is written once, by the `Init` that establishes it, and never
         * again; `let &x` is what licenses the second write. So the binding's permission is checked
         * against the *destination mode* and not on its own.
         */
        if(written.mode != Destination::Init &&
           written.destination.source() == SourceKind::Place) {
            auto place = body.place(base, written.destination.placeId());
            if(!place->writable) {
                report(node.source, "internal: write.%@ names a binding that may not be assigned"_v,
                       destinationName(written.mode));
            }
        }

        Occurrence destination, value;
        if(!occurrenceOf(written.destination, destination)) return;
        if(!occurrenceOf(written.value, value)) return;

        if(!sameType(destination.type, value.type)) {
            report(node.source, "internal: write stores %@ into storage holding %@"_v,
                   typeText(decl, value.type), typeText(decl, destination.type));
        }
    }

    void checkBlock(Block& block) {
        checkArguments(block);

        auto count = block.nodes.size();

        if(!count) {
            report(kNullLocation, "internal: block %@ is empty"_v, block.index.index);
            return;
        }

        for(Size i = 0; i < count; i++) {
            auto node = base[block.nodes.get(base, i)];
            auto last = i + 1 == count;

            if(isTerminator(node->kind) != last) {
                report(node->source, last
                    ? "internal: block %@ does not end in a terminator"_v
                    : "internal: block %@ has a terminator in the middle"_v, block.index.index);
            }

            if(node->id.index >= body.nodes.size() ||
               body.nodes.get(base, node->id.index) != node - base) {
                report(node->source, "internal: %@ is not stored at the ID it records"_v,
                       nodeMnemonic(node->kind));
            }

            if(hasResult(node->kind) && !valueOccurrence(*node).type) {
                report(node->source, "internal: %@ produces a value with no type"_v,
                       nodeMnemonic(node->kind));
            }

            forEachOperand(*node, [&](Operand& operand, OperandRole role) {
                if(checkOperand(*node, operand)) checkRole(*node, operand, role);
            });

            checkNode(*node);

            forEachEdge(*node, [&](Edge& edge) { checkEdge(*node, edge); });
        }
    }

    bool run() {
        if(!body.sealed) {
            report(function.source, "internal: the body of %@ was verified before it was sealed"_v,
                   context.findName(function.name));
            return false;
        }

        checkPlaces();
        for(U32 i = 0; i < body.blocks.size(); i++) checkBlock(*body.block(base, BlockId(i)));

        // The entry may not be a merge: its arguments are the function's, and a predecessor
        // supplying them would mean the function had two ways in.
        auto entry = base[body.entry];
        if(entry->incoming.size()) {
            report(function.source, "internal: the entry block of %@ has predecessors"_v,
                   context.findName(function.name));
        }

        return errors == 0;
    }
};

} // namespace

bool verifyBody(Context& context, Program& program, Function& function) {
    if(!function.mir) return true;

    Verifier verifier {
        context, program, function, *(*program.mir)[function.mir], *program.mir, *program.decl
    };

    return verifier.run();
}

bool verifyProgram(Context& context, Program& program) {
    auto declBase = *program.decl;
    auto valid = true;

    for(auto modulePointer: program.modules) {
        auto module = declBase[modulePointer];
        for(auto functionPointer: module->functions.contents(declBase)) {
            valid &= verifyBody(context, program, *declBase[functionPointer]);
        }
    }

    return valid;
}

} // namespace mir
