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
             * Nothing here about the *storage*, and that is the point of the split.
             *
             * Both remaining questions about a destination need the write's `Destination` in hand,
             * and this walk does not have it: a `let x` place is not writable and is still written
             * exactly once by the `Init` that establishes it, and a `let d = c` place holds a shared
             * loan and is still established by an `Init` that fills the slot rather than writing
             * through the loan. `checkWrite` is where both are asked, where the field is.
             */
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

            /*
             * The relocation sites, which are exactly these.
             *
             * A `Take` out of a place is the object leaving the storage it was in - into an
             * argument, into a return, into another local - and that is the whole enumeration,
             * because every other operand either copies (a second object) or borrows (no travel).
             * A pinned type is one somebody else holds the address of, so this is where it is
             * refused. Nothing classifies as pinned yet; the check is here rather than with the
             * attribute so that arriving at the feature is a fold in `classifyTypes` and not a
             * search for the places a move can happen.
             */
            if(!isRelocatable(decl, place->occurrence.type)) {
                report(node.source, "internal: %@ moves a %@, which may not be relocated"_v,
                       mnemonic, typeText(context, decl, place->occurrence.type));
            }
        }
    }

    /*
     * The object table and the lineage relation over it - `Object`.
     *
     * Three statements, and the third is the one a merge can break: the side table stays dense over
     * node IDs, every id names a row that exists, and following `parent` terminates. A cycle is what
     * a union that skipped `objectRoot` produces, and the symptom of one is a hang in whatever asks
     * where something lives rather than a wrong answer - which is the worst kind of defect to leave
     * to a later pass to discover.
     */
    void checkObjects() {
        if(body.nodeObject.size() != body.nodes.size()) {
            report(kNullLocation,
                   "internal: the object side table has %@ entries for %@ nodes"_v,
                   U64(body.nodeObject.size()), U64(body.nodes.size()));
        }

        for(U32 i = 0; i < body.objects.size(); i++) {
            auto object = body.object(base, ObjectId(i));

            if(object->id.index != i) {
                report(object->source, "internal: object %@ is stored at index %@"_v,
                       object->id.index, i);
                continue;
            }

            if(!object->parent.valid() || object->parent.index >= body.objects.size()) {
                report(object->source, "internal: object %@ names a lineage that does not exist"_v, i);
                continue;
            }

            // Bounded by the table size, because a chain longer than that has visited a row twice.
            auto steps = 0u;
            auto at = object->parent;
            while(body.object(base, at)->parent != at) {
                at = body.object(base, at)->parent;

                if(++steps <= body.objects.size()) continue;

                report(object->source, "internal: object %@ is in a lineage cycle"_v, i);
                break;
            }
        }

        for(U32 i = 0; i < body.nodes.size() && i < body.nodeObject.size(); i++) {
            auto id = body.objectOf(base, NodeId(i));
            if(!id.valid() || id.index < body.objects.size()) continue;

            report(body.node(base, NodeId(i))->source,
                   "internal: node %@ names object %@, which does not exist"_v, i, id.index);
        }

        /*
         * A region placement without a witness to place it in - §10.4, and the seam this check is.
         *
         * `StorageClass::Region` means "in the region the ambient witness names", and the witness is
         * a member of the *generic environment*: a declared parameter a caller substitutes, not
         * something a lowering conjures because it noticed an object outlives its frame. There is no
         * generic environment in this stage yet, so there is no witness, so nothing may select this
         * rung - and the way that is stated is a check that fires rather than a comment.
         *
         * What it buys is the day it stops being true. An escape analysis that started choosing
         * `Region` before the environment carried a witness would be synthesizing the region, and
         * the symptom of that is a placement decision with nothing to place into - which is a
         * miscompile at the far end rather than a report here.
         */
        for(U32 i = 0; i < body.objects.size(); i++) {
            auto object = body.object(base, ObjectId(i));
            if(object->storage != StorageClass::Region) continue;

            report(object->source,
                   "internal: object %@ is placed in a region, and no witness declares one"_v, i);
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

                /*
                 * A projection names a field its parent's type has, and holds that field's type.
                 *
                 * Both halves, because the payload and the occurrence are two records of one fact:
                 * `Builder::fieldPlace` derives the second from the first, and a place built any
                 * other way would be a projection whose type has nothing to do with what is there.
                 */
                if(place->kind == Place::Field) {
                    auto holder = parent->occurrence.type;

                    if(!holder || decl[holder]->kind != Type::Record) {
                        report(kNullLocation, "internal: place %@ projects a field of %@"_v, i,
                               typeText(context, decl, holder));
                    } else if(place->payload >= ((RecordType*)decl[holder])->fields.size()) {
                        report(kNullLocation, "internal: place %@ names field %@ of %@"_v, i,
                               place->payload, typeText(context, decl, holder));
                    } else if(!sameType(place->occurrence.type,
                                        ((RecordType*)decl[holder])->field(decl, U16(place->payload)).type)) {
                        report(kNullLocation, "internal: place %@ records the wrong field type"_v, i);
                    }
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
                       nodeMnemonic(node.kind), i, typeText(context, decl, supplied.type),
                       target->index.index, typeText(context, decl, declared.type));
                continue;
            }

            if(supplied.contract != declared.contract || supplied.shape != declared.shape) {
                report(node.source,
                       "internal: %@ hands argument %@ to block %@ under a contract it does not declare"_v,
                       nodeMnemonic(node.kind), i, target->index.index);
            }
        }
    }

    /*
     * A `return` against the result this function declared.
     *
     * Both directions, because they are two different failures: a value where the signature says
     * `{}` is a function handing back something no caller has storage for, and no value where the
     * signature says otherwise is a caller reading a register nothing wrote. Neither has a symptom
     * before the machine boundary.
     */
    void checkReturn(Node& node) {
        auto& written = (NodeReturn&)node;
        auto unit = decl[function.result.type]->kind == Type::Unit;

        if(written.hasValue == unit) {
            report(node.source, unit
                ? "internal: return hands back a value, and this function answers {}"_v
                : "internal: return hands back nothing, and this function answers a value"_v);
            return;
        }

        if(!written.hasValue) return;

        Occurrence handed;
        if(!occurrenceOf(written.value, handed)) return;

        if(!sameType(handed.type, function.result.type)) {
            report(node.source, "internal: return hands back %@, and this function answers %@"_v,
                   typeText(context, decl, handed.type), typeText(context, decl, function.result.type));
        }
    }

    /*
     * A call against the signature it names - §5.6.
     *
     * The arity, the argument types and the *modes* - because a mode is what the callee receives the
     * argument as, and a call that hands a borrow where the signature declared a transfer is the
     * ownership bug this whole representation exists to make sayable. `argumentMode` is the one rule
     * and this restates it over the built node, so a builder assertion and a diagnostic in every
     * build are the two halves of the same statement.
     */
    void checkCall(Node& node) {
        auto& written = (NodeCall&)node;
        auto callee = decl[written.callee];

        if(!callee) {
            report(node.source, "internal: call names no callee"_v);
            return;
        }

        if(written.arguments.size() != callee->args.size()) {
            report(node.source, "internal: call to %@ supplies %@ arguments, and it declares %@"_v,
                   context.findName(callee->name), written.arguments.size(), callee->args.size());
            return;
        }

        for(U32 i = 0; i < written.arguments.size(); i++) {
            auto operand = written.arguments.get(base, i);
            auto& parameter = *decl[callee->args.get(decl, i)];

            Occurrence supplied;
            if(!occurrenceOf(operand, supplied)) continue;

            if(!sameType(supplied.type, parameter.occurrence.type)) {
                report(node.source,
                       "internal: call to %@ supplies argument %@ of type %@, and it declares %@"_v,
                       context.findName(callee->name), i, typeText(context, decl, supplied.type),
                       typeText(context, decl, parameter.occurrence.type));
                continue;
            }

            auto wanted = argumentMode(parameter.occurrence.contract, operand.source());
            if(operand.mode() != wanted) {
                report(node.source,
                       "internal: call to %@ hands argument %@ over as %@, and it declares %@"_v,
                       context.findName(callee->name), i, useModeName(operand.mode()),
                       useModeName(wanted));
            }
        }

        // The result the caller believes it received. Nothing substitutes an origin slot yet, so a
        // callee that published one would leave every caller's occurrence naming a scope it is not
        // in - which is a wrong answer rather than a missing one.
        if(valueOccurrence(node) != callee->result) {
            report(node.source, "internal: call to %@ records a result its signature does not"_v,
                   context.findName(callee->name));
        }
    }

    /*
     * An aggregate against the record it builds - §5.7.
     *
     * The count and the component types, on the same terms as a call against its signature: the
     * modes are not checked because they are not supplied - `argumentMode` produces them for an
     * owned position - and the *record* is what says how many there are. A component list that
     * disagreed with its type would otherwise arrive as a field written past the end of an
     * allocation.
     */
    void checkAggregate(Node& node) {
        auto& written = (NodeAggregate&)node;
        auto type = valueOccurrence(node).type;

        if(!type || decl[type]->kind != Type::Record) {
            report(node.source, "internal: aggregate produces %@, which is not a record"_v,
                   typeText(context, decl, type));
            return;
        }

        auto record = (RecordType*)decl[type];
        if(written.components.size() != record->fields.size()) {
            report(node.source, "internal: aggregate supplies %@ components, and %@ declares %@"_v,
                   written.components.size(), typeText(context, decl, type),
                   record->fields.size());
            return;
        }

        for(U32 i = 0; i < written.components.size(); i++) {
            auto operand = written.components.get(base, i);
            auto declared = record->field(decl, U16(i));

            Occurrence supplied;
            if(!occurrenceOf(operand, supplied)) continue;

            if(!sameType(supplied.type, declared.type)) {
                report(node.source,
                       "internal: aggregate supplies component %@ of type %@, and the field is %@"_v,
                       i, typeText(context, decl, supplied.type),
                       typeText(context, decl, declared.type));
                continue;
            }

            // The field's own contract, restated - `Builder::addAggregate` reads the same one. A
            // component filling an external field is borrowed into the record rather than taken.
            auto wanted = argumentMode(declared.contract, operand.source());
            if(operand.mode() != wanted) {
                report(node.source,
                       "internal: aggregate installs component %@ as %@, and a field is %@"_v,
                       i, useModeName(operand.mode()), useModeName(wanted));
            }
        }
    }

    void checkWrite(Node& node) {
        auto& written = (NodeWrite&)node;

        /*
         * An assignment is not an initialization, which is the distinction this whole node exists
         * for - §5.5. A `let x` place is written once, by the `Init` that establishes it, and never
         * again; `let &x` is what licenses the second write. So the binding's permission is checked
         * against the *destination mode* and not on its own.
         */
        if(written.destination.source() == SourceKind::Place) {
            auto place = body.place(base, written.destination.placeId());

            if(written.mode != Destination::Init && !place->writable) {
                report(node.source, "internal: write.%@ names a binding that may not be assigned"_v,
                       destinationName(written.mode));
            }

            /*
             * Writing *through* a place is exclusive access to what is in it, and an `Init` is not
             * that: it fills a slot this frame owns, and what the slot then holds may perfectly well
             * be a shared loan - `let d = c` is exactly that and is what the language says a plain
             * binding of a name is. So the access is checked for every write but the one that
             * establishes storage, and `Builder::addWrite` states the same pair from the other side.
             */
            if(written.mode != Destination::Init &&
               place->occurrence.contract.access() == Access::Shared) {
                report(node.source,
                       "internal: write.%@ writes through a place this function only shares"_v,
                       destinationName(written.mode));
            }

            // An `Init` establishes a slot, and the only storage this frame may establish is its
            // own. A global or a projection of a parameter is filled by somebody else.
            if(written.mode == Destination::Init) {
                auto root = place;
                while(root->parent) root = base[root->parent];

                if(root->kind != Place::Local) {
                    report(node.source, "internal: write.init establishes %@, which is not a local"_v,
                           placeKindName(root->kind));
                }
            }
        }

        Occurrence destination, value;
        if(!occurrenceOf(written.destination, destination)) return;
        if(!occurrenceOf(written.value, value)) return;

        if(!sameType(destination.type, value.type)) {
            report(node.source, "internal: write stores %@ into storage holding %@"_v,
                   typeText(context, decl, value.type), typeText(context, decl, destination.type));
        }
    }

    /*
     * The rules that are about one node's own fields rather than about one operand.
     *
     * A kind with no case here is one whose whole content is its operands, and `checkRole` is what
     * checks those - so the absence of a row is a statement rather than an omission. What belongs
     * here is a rule relating two of a node's own fields, or one of its fields to something outside
     * the body: a destination mode against the binding it names, a call against its callee's
     * signature, a `return` against the function's result.
     */
    void checkNode(Node& node) {
        switch(node.kind) {
            case Node::Aggregate: return checkAggregate(node);
            case Node::Write:  return checkWrite(node);
            case Node::Call:   return checkCall(node);
            case Node::Return: return checkReturn(node);
            default:           return;
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

            forEachOperand(base, *node, [&](Operand& operand, OperandRole role) {
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
        checkObjects();
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
