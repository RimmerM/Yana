#include "verify.h"
#include "specialize.h"

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
         * something a lowering conjures because it noticed an object outlives its frame. A
         * `GenContext` exists now and binds type variables; what it has no slot kind for is a
         * region, so there is still no witness and nothing may select this rung - and the way that
         * is stated is a check that fires rather than a comment.
         *
         * What it buys is the day it stops being true. An escape analysis that started choosing
         * `Region` before the environment carried a witness would be synthesizing the region, and
         * the symptom of that is a placement decision with nothing to place into - which is a
         * miscompile at the far end rather than a report here.
         *
         * `StorageClass::Heap` is the other rung and is refused in the same walk, for the same
         * sentence with a different missing half: what it needs is an allocator. A **closure whose
         * environment escapes** is the first thing in the language that asks for one
         * (`resolve_lambda.cpp`), and every path that would select it reports where the escape is
         * written instead. So this is the backstop at the point the allocation would have to be
         * emitted - something that selected the rung anyway would otherwise reach the lowering,
         * which allocates a frame slot whatever the object says.
         */
        for(U32 i = 0; i < body.objects.size(); i++) {
            auto object = body.object(base, ObjectId(i));

            if(object->storage == StorageClass::Region) {
                report(object->source,
                       "internal: object %@ is placed in a region, and no witness declares one"_v, i);
            }

            if(object->storage == StorageClass::Heap) {
                report(object->source,
                       "internal: object %@ is placed on the heap, and this stage has no allocator"_v,
                       i);
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

                /*
                 * A projection names a field its parent's type has, and holds that field's type.
                 *
                 * Both halves, because the payload and the occurrence are two records of one fact:
                 * `Builder::fieldPlace` derives the second from the first, and a place built any
                 * other way would be a projection whose type has nothing to do with what is there.
                 */
                /*
                 * A property step names a requirement of this body's own context, and holds what
                 * that requirement said the field is - the same two halves the field check below
                 * compares, read out of the declaration instead of out of a record.
                 */
                if(place->kind == Place::Property) {
                    auto owner = body.gen ? decl[body.gen] : nullptr;

                    if(!owner || place->payload >= owner->properties.size()) {
                        report(kNullLocation,
                               "internal: place %@ names requirement %@, which this signature does not state"_v,
                               i, place->payload);
                    } else if(decl[parent->occurrence.type]->kind != Type::Variable) {
                        report(kNullLocation,
                               "internal: place %@ reaches a requirement through %@"_v, i,
                               typeText(context, decl, parent->occurrence.type));
                    } else {
                        auto required = owner->properties.get(decl, U32(place->payload));
                        if(required.subject != parent->occurrence.type ||
                           !sameType(place->occurrence.type, required.result)) {
                            report(kNullLocation,
                                   "internal: place %@ records the wrong requirement"_v, i);
                        }
                    }
                }

                if(place->kind == Place::Field) {
                    auto holder = parent->occurrence.type;

                    if(!holder || decl[holder]->kind != Type::Record) {
                        report(kNullLocation, "internal: place %@ projects a field of %@"_v, i,
                               typeText(context, decl, holder));
                    } else if(place->payload >= ((RecordType*)decl[holder])->fields.size()) {
                        report(kNullLocation, "internal: place %@ names field %@ of %@"_v, i,
                               place->payload, typeText(context, decl, holder));
                    } else if(!sameType(place->occurrence.type,
                                        ((RecordType*)decl[holder])->field(decl, U16(place->payload))
                                            .occurrence.type)) {
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

        // The edges whose targets' parameters are not filled from this block - see `checkLensCall`,
        // which checks them against the callee's declared hand-over and failure instead. Both of a
        // `lens.try`'s: what arrives on either is produced *inside* the call, so there is no value
        // in this block for either edge to name.
        if(isLensCall(node.kind)) return;

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
     * A call through a function value - the builder's rule restated over the built node.
     *
     * Exactly `checkCall`'s statements with the signature read out of the target's type rather
     * than a declaration: the arity, each argument's type, and each argument's mode. What is *not*
     * restated is a type-argument list, because a function type binds no variables - a value of one
     * is a single signature and there is nothing left open in it.
     */
    void checkValueCall(Node& node) {
        auto& written = (NodeCall&)node;

        Occurrence target;
        if(!occurrenceOf(written.target, target)) return;

        if(!target.type || decl[target.type]->kind != Type::Fun) {
            report(node.source, "internal: a call through a value whose type is %@"_v,
                   typeText(context, decl, target.type));
            return;
        }

        // `Inspect` and nothing else - `addCallValue` says why: a call reads two words out of the
        // pair and takes neither the value nor what it holds.
        if(written.target.mode() != UseMode::Inspect) {
            report(node.source, "internal: a call reads its callee as %@ rather than inspecting it"_v,
                   useModeName(written.target.mode()));
        }

        auto& shape = *(FunType*)decl[target.type];
        if(written.arguments.size() != shape.params.size()) {
            report(node.source, "internal: a call through %@ supplies %@ arguments"_v,
                   typeText(context, decl, target.type), written.arguments.size());
            return;
        }

        for(U32 i = 0; i < written.arguments.size(); i++) {
            auto operand = written.arguments.get(base, i);
            auto declared = shape.params.get(decl, i).occurrence;

            Occurrence supplied;
            if(!occurrenceOf(operand, supplied)) continue;

            if(!sameType(supplied.type, declared.type)) {
                report(node.source,
                       "internal: a call through %@ supplies argument %@ of type %@, and it declares %@"_v,
                       typeText(context, decl, target.type), i,
                       typeText(context, decl, supplied.type),
                       typeText(context, decl, declared.type));
                continue;
            }

            auto wanted = argumentMode(declared.contract, operand.source());
            if(operand.mode() != wanted) {
                report(node.source,
                       "internal: a call through %@ passes argument %@ as %@, and its position asks for %@"_v,
                       typeText(context, decl, target.type), i, useModeName(operand.mode()),
                       useModeName(wanted));
            }
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

        // A call typed against a **requirement** is checked against that instead - see below. The
        // callee's own signature is not what it was built from, so restating the builder's rule
        // over it would be restating a different rule.
        if(written.genCallable != NodeCall::kNoCallable) return checkRequiredCall(node);

        // A call through a function **value** is checked against the value's type, which is the
        // signature there is: there is no declaration, and the shape the target carries is what the
        // builder read the modes and the result off.
        if(written.callsValue()) return checkValueCall(node);

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

        /*
         * The signature read through the call's own type arguments - `gen.h`.
         *
         * The same `substituteType` the builder used, read a second time over the built node, which
         * is the pattern `argumentMode` is already under: the rule is written once and restated
         * where the result of applying it can be checked. A call whose arguments were substituted
         * with one list and whose result was recorded from another is exactly the mistake a
         * specialization would then carry into a backend.
         */
        auto variables = callee->gen ? decl[callee->gen]->variables.size() : 0;
        if(written.typeArguments.size() != variables) {
            report(node.source, "internal: call to %@ supplies %@ type arguments, and it binds %@"_v,
                   context.findName(callee->name), written.typeArguments.size(), variables);
            return;
        }

        SmallArray<DeclPtr<Type>, 4> typeArguments;
        for(auto argument: written.typeArguments.contents(base)) typeArguments.push(argument);

        Substitution with { callee->gen, typeArguments.pointer(), typeArguments.size() };

        for(U32 i = 0; i < written.arguments.size(); i++) {
            auto operand = written.arguments.get(base, i);
            auto& parameter = *decl[callee->args.get(decl, i)];
            auto declared = substituteType(context, program, parameter.occurrence.type, with);

            Occurrence supplied;
            if(!occurrenceOf(operand, supplied)) continue;

            if(!sameType(supplied.type, declared)) {
                report(node.source,
                       "internal: call to %@ supplies argument %@ of type %@, and it declares %@"_v,
                       context.findName(callee->name), i, typeText(context, decl, supplied.type),
                       typeText(context, decl, declared));
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

        // The result the caller believes it received, substituted the same way the arguments were.
        // Nothing substitutes an origin slot yet, so a callee that published one would leave every
        // caller's occurrence naming a scope it is not in - which is a wrong answer rather than a
        // missing one.
        if(valueOccurrence(node) != substituteOccurrence(context, program, callee->result, with)) {
            report(node.source, "internal: call to %@ records a result its signature does not"_v,
                   context.findName(callee->name));
        }
    }

    /*
     * A call typed against `f: (a) -> b` - `NodeCall::genCallable`.
     *
     * The same three statements the check above makes, against the promise instead of against a
     * declaration: one value per position, at the type the requirement stated, handed over the way a
     * requirement's positions bind. There is nothing to substitute - a requirement is written in the
     * enclosing signature's own variables, and this body *is* that signature.
     *
     * Which function it reaches is deliberately not checked here. That is `callTarget`'s answer and
     * it differs per specialization, so a check on the generic body could only assert the first of
     * them.
     */
    void checkRequiredCall(Node& node) {
        auto& written = (NodeCall&)node;
        auto owner = body.gen ? decl[body.gen] : nullptr;

        if(!owner || written.genCallable >= owner->callables.size()) {
            report(node.source,
                   "internal: call names requirement %@, which this signature does not state"_v,
                   written.genCallable);
            return;
        }

        auto required = owner->callables.get(decl, written.genCallable);

        if(written.arguments.size() != required.args.size()) {
            report(node.source,
                   "internal: call to %@ supplies %@ arguments, and the requirement states %@"_v,
                   context.findName(required.name), written.arguments.size(),
                   required.args.size());
            return;
        }

        for(U32 i = 0; i < written.arguments.size(); i++) {
            auto operand = written.arguments.get(base, i);
            auto declared = required.args.get(decl, i);

            Occurrence supplied;
            if(!occurrenceOf(operand, supplied)) continue;

            if(!sameType(supplied.type, declared)) {
                report(node.source,
                       "internal: call to %@ supplies argument %@ of type %@, and the requirement states %@"_v,
                       context.findName(required.name), i, typeText(context, decl, supplied.type),
                       typeText(context, decl, declared));
                continue;
            }

            auto wanted = argumentMode(borrowedContract(Access::Shared), operand.source());
            if(operand.mode() != wanted) {
                report(node.source,
                       "internal: call to %@ hands argument %@ over as %@, and it binds %@"_v,
                       context.findName(required.name), i, useModeName(operand.mode()),
                       useModeName(wanted));
            }
        }

        if(valueOccurrence(node) != owned(required.result)) {
            report(node.source, "internal: call to %@ records a result the requirement does not"_v,
                   context.findName(required.name));
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

        auto kind = type ? decl[type]->kind : Type::KindCount;
        if(kind != Type::Record && kind != Type::Array) {
            report(node.source,
                   "internal: aggregate produces %@, which is neither a record nor a fixed array"_v,
                   typeText(context, decl, type));
            return;
        }

        if(kind == Type::Record && written.constructor >= ((RecordType*)decl[type])->cons.size()) {
            report(node.source, "internal: aggregate builds constructor %@, and %@ declares %@"_v,
                   U64(written.constructor), typeText(context, decl, type),
                   U64(((RecordType*)decl[type])->cons.size()));
            return;
        }

        // The components are one constructor's fields, which is every field of a record with one
        // constructor - `NodeAggregate::constructor` - and every element of a fixed array.
        // `aggregateComponentCount` is the one place that split is made.
        auto expected = aggregateComponentCount(decl, type, written.constructor);
        if(written.components.size() != expected) {
            report(node.source, "internal: aggregate supplies %@ components, and %@ holds %@"_v,
                   written.components.size(), typeText(context, decl, type), U64(expected));
            return;
        }

        for(U32 i = 0; i < written.components.size(); i++) {
            auto operand = written.components.get(base, i);
            auto declared = aggregateComponent(decl, type, written.constructor, i);

            Occurrence supplied;
            if(!occurrenceOf(operand, supplied)) continue;

            if(!sameType(supplied.type, declared.type)) {
                report(node.source,
                       "internal: aggregate supplies component %@ of type %@, and the position is %@"_v,
                       i, typeText(context, decl, supplied.type),
                       typeText(context, decl, declared.type));
                continue;
            }

            // The position's own contract, restated - `Builder::addAggregate` reads the same one. A
            // component filling an external field is borrowed into the record rather than taken.
            auto wanted = argumentMode(declared.contract, operand.source());
            if(operand.mode() != wanted) {
                report(node.source,
                       "internal: aggregate installs component %@ as %@, and the position is %@"_v,
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

            /*
             * An `Init` establishes a slot, and the two kinds of slot that have a first write are a
             * local of this frame and a **global**.
             *
             * A projection of a parameter is filled by somebody else, and so is a loan or a raw
             * address; a global is filled by the root module's top level and by nothing else
             * (Analysis-Initialization.md stage B), which is a first write with no earlier value to
             * release - see `Builder::addWrite`, where the same pair is stated from the other side.
             */
            if(written.mode == Destination::Init) {
                auto root = place;
                while(root->parent) root = base[root->parent];

                if(root->kind != Place::Local && root->kind != Place::Global) {
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
    /*
     * The continuation regions, and the nesting rule over them - `Continuation`.
     *
     * Four statements: the table is a forest with one root, every block names a region that exists,
     * a `Handover` says what it hands over and the other kinds do not, and a region that runs at
     * most once is not re-entered from within itself. The edge half of the rule is `checkEdgeRegion`
     * below, because that is where the terminators are.
     *
     * The same four `ssa::verifyBody` makes, deliberately, and this is the tier that can be wrong
     * first: a region here is what a *builder* said while it was emitting, and the conversion below
     * transcribes it. A rule checked only after the transcription would be reporting the copy.
     */
    void checkRegions() {
        if(!body.regions.size()) {
            report(function.source, "internal: %@ has no continuation region"_v,
                   context.findName(function.name));
            return;
        }

        for(U32 i = 0; i < body.regions.size(); i++) {
            auto region = body.region(base, ContinuationId(i));

            if(region->id.index != i) {
                report(function.source, "internal: continuation region %@ is stored at index %@"_v,
                       region->id.index, i);
            }

            // The root is region zero and has no parent; every other names one made before it,
            // which is what makes the table a forest rather than something with a cycle in it.
            if(i == 0) {
                if(region->parent.valid()) {
                    report(function.source, "internal: the root continuation region has a parent"_v);
                }
            } else if(!region->parent.valid() || region->parent.index >= i) {
                report(function.source,
                       "internal: continuation region %@ names an enclosing region that is not"_v, i);
            }

            // A hand-over is the one kind that receives something, so it is the one kind with a
            // declared occurrence - see `Continuation::handover`.
            auto hands = region->kind == Continuation::Handover;
            if(hands == (region->handover.type != nullptr)) continue;

            report(function.source, hands
                ? "internal: continuation region %@ hands over and declares no occurrence"_v
                : "internal: continuation region %@ declares a hand-over it does not perform"_v, i);
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));
            if(block->region.valid() && block->region.index < body.regions.size()) continue;

            report(function.source, "internal: block %@ names no continuation region"_v, i);
        }
    }

    /*
     * One edge against the nesting rule - see `checkRegions`.
     *
     * An edge either stays inside a region, enters a **direct child at its entry**, or leaves for
     * the destination that child declared - and the asymmetry between the two is the point. A region
     * is entered at one place by definition, which is what makes it a continuation rather than a
     * subgraph; it is not *left* at one by definition, since §10.1's continuation has four exits, so
     * `destination` is optional and the rule holds a region only to what its builder said.
     */
    void checkEdgeRegion(Node& node, Block& from, Block& target) {
        if(!from.region.valid() || !target.region.valid()) return;

        /*
         * A `break` that stays inside its own region, which is the one thing the kind cannot mean.
         *
         * `Break` and `Jump` are the same edge and differ only in the row of the exit signal they
         * become, so a `break` within one region is a jump written under the wrong name - and the
         * conversion would emit an exit from a scope that is not leaving. `break` in a loop of the
         * caller's own blocks is built as a `Jump`, which is what makes this a check rather than a
         * case to handle.
         */
        if(from.region == target.region) {
            if(node.kind == Node::Break) {
                report(node.source,
                       "internal: a break stays inside continuation region %@, so it is a jump"_v,
                       from.region.index);
            }

            return;
        }

        auto region = body.region(base, target.region);

        if(region && region->parent == from.region) {
            if(!region->entry.valid() || region->entry == target.index) return;

            report(node.source,
                   "internal: %@ enters continuation region %@ at block %@, which is not its entry %@"_v,
                   nodeMnemonic(node.kind), target.region.index, target.index.index,
                   region->entry.index);
            return;
        }

        if(body.encloses(base, target.region, from.region)) {
            auto leaving = body.region(base, from.region);
            if(!leaving || !leaving->destination.valid()) return;
            if(leaving->destination == target.index) return;

            report(node.source,
                   "internal: %@ leaves continuation region %@ for block %@, which is not the destination %@ it rejoins at"_v,
                   nodeMnemonic(node.kind), from.region.index, target.index.index,
                   leaving->destination.index);
            return;
        }

        report(node.source,
               "internal: %@ leaves continuation region %@ for %@, which neither encloses it nor is entered from it"_v,
               nodeMnemonic(node.kind), from.region.index, target.region.index);
    }

    /*
     * A lens call against the region it opens.
     *
     * Four things the builder asserted while it was making them, restated where a body that arrived
     * from anywhere else is checked: the region exists and is a hand-over, it is a direct child of
     * the block's own, its entry is where this edge goes, and it declares one parameter for what is
     * handed over. The last is what makes the hand-over a value at all - a region entered with
     * nothing to receive is a jump.
     *
     * Where it *rejoins* is deliberately not among them. A continuation every path of which leaves
     * has a destination nothing enters, `seal` prunes it, and the region legitimately has none -
     * which is why `Continuation::destination` is optional where `entry` is not.
     *
     * The entry's parameter is deliberately **not** an operand of this node and cannot be: what
     * arrives is the callee's, produced inside a call this frame is in the middle of. So it is
     * checked against `Continuation::handover` - the declaration - rather than against an edge
     * operand, which is the one place an edge and its target's parameters are related by something
     * other than the operand list.
     */
    void checkLensCall(Node& node, Block& from) {
        auto& written = (NodeLensCall&)node;
        auto region = body.region(base, written.region);

        if(!region || region->kind != Continuation::Handover) {
            report(node.source, "internal: lens.call names %@, which is not a hand-over region"_v,
                   written.region.index);
            return;
        }

        if(region->parent != from.region) {
            report(node.source,
                   "internal: lens.call in region %@ opens region %@, which is enclosed by %@"_v,
                   from.region.index, written.region.index, region->parent.index);
        }

        if(!written.callee) {
            report(node.source, "internal: lens.call names no callee"_v);
        } else if(!decl[written.callee]->lens) {
            report(node.source, "internal: lens.call names %@, which is not a lens"_v,
                   context.findName(decl[written.callee]->name));
        } else if(written.arguments.size() != decl[written.callee]->writtenArity()) {
            report(node.source, "internal: lens.call passes %@ arguments to a lens taking %@"_v,
                   Size(written.arguments.size()), Size(decl[written.callee]->writtenArity()));
        } else if((node.kind == Node::LensTry) != (decl[written.callee]->failure != nullptr)) {
            /*
             * The kind against the callee's signature, which is the whole of what separates the two
             * rows: a skipping callee has a second way to finish and therefore a second edge, and a
             * transparent one has neither. Checked rather than assumed because the resolver picks
             * the kind from the same field, and a check that reads what the writer read is what
             * catches the writer having read it at the wrong moment.
             */
            report(node.source, "internal: %@ calls %@, whose failure channel says the other kind"_v,
                   nodeMnemonic(node.kind), context.findName(decl[written.callee]->name));
        }

        if(!written.to.target) return;

        auto target = base[written.to.target];
        if(region->entry != target->index) {
            report(node.source, "internal: lens.call enters region %@ at block %@ rather than its entry %@"_v,
                   written.region.index, target->index.index, region->entry.index);
        }

        if(target->arguments.size() != 1) {
            report(node.source,
                   "internal: lens.call hands over to block %@, which declares %@ parameters rather than one"_v,
                   target->index.index, target->arguments.size());
            return;
        }

        auto& handed = *(NodeResult*)base[target->arguments.get(base, 0)];
        if(!(handed.occurrence == region->handover)) {
            report(node.source,
                   "internal: lens.call hands over %@, and its region declares %@"_v,
                   typeText(context, decl, handed.occurrence.type),
                   typeText(context, decl, region->handover.type));
        }

        if(node.kind != Node::LensTry) return;

        /*
         * The skip edge, whose one argument is what the callee failed with.
         *
         * Checked here and not by `checkEdge` for the half `checkEdge` cannot see: the *argument* on
         * that edge is filled from this block and is checked there, and what it has to agree with is
         * the callee's declared `| E`, which only this node knows.
         */
        if(!written.otherwise.target) {
            report(node.source, "internal: lens.try has no edge for the traversal failing"_v);
            return;
        }

        auto skipped = base[written.otherwise.target];
        if(skipped->region != from.region) {
            report(node.source,
                   "internal: lens.try skips into region %@ from region %@"_v,
                   skipped->region.index, from.region.index);
        }

        if(skipped->arguments.size() != 1) {
            report(node.source,
                   "internal: lens.try skips to block %@, which declares %@ parameters rather than one"_v,
                   skipped->index.index, skipped->arguments.size());
            return;
        }

        auto& reason = *(NodeResult*)base[skipped->arguments.get(base, 0)];
        if(!(reason.occurrence == written.reason)) {
            report(node.source, "internal: lens.try skips with %@, and the call declares %@"_v,
                   typeText(context, decl, reason.occurrence.type),
                   typeText(context, decl, written.reason.type));
        }
    }

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
            if(isLensCall(node->kind)) checkLensCall(*node, block);

            forEachEdge(*node, [&](Edge& edge) {
                checkEdge(*node, edge);
                if(edge.target) checkEdgeRegion(*node, block, *base[edge.target]);
            });
        }
    }

    /*
     * Every value operand names a node of the *same* block - §5.9's half of block arguments.
     *
     * The representation permits more than that: an operand is a node ID and `checkOperand` holds it
     * to naming a node created earlier, which is an ordering statement and not a scoping one. What
     * makes the stronger rule true is that a merge is spelled with block arguments, so a value that
     * has to survive a branch is handed over on the edge rather than named across it.
     *
     * Checked rather than assumed, because two things now depend on it. The origin fixpoint wakes a
     * block's *successors* when its exit state or one of their parameters moves, and a value read
     * across a block boundary would be a fourth way for one block's answer to reach another - one
     * the worklist does not follow. And a body where it held only by accident would make the SSA
     * conversion's node-to-value map a dominance question rather than a lookup.
     */
    void checkValueScope() {
        SmallArray<U32, 128> owner;
        for(U32 i = 0; i < body.nodes.size(); i++) owner.push(~U32(0));

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));

            for(auto argument: block->arguments.contents(base)) owner[base[argument]->id.index] = i;
            for(auto node: block->nodes.contents(base)) owner[base[node]->id.index] = i;
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));

            for(auto nodePointer: block->nodes.contents(base)) {
                auto& node = *base[nodePointer];

                forEachOperand(base, node, [&](Operand& operand, OperandRole) {
                    if(operand.source() != SourceKind::Result) return;
                    if(operand.id() >= body.nodes.size()) return;
                    if(owner[operand.id()] == i) return;

                    report(node.source,
                           "internal: %@ names result %@, which belongs to block %@ - a value that "
                           "crosses a block boundary is handed over as a block argument"_v,
                           nodeMnemonic(node.kind), operand.id(), owner[operand.id()]);
                });
            }
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
        checkRegions();
        checkValueScope();
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

/*
 * One specialization against the generic it came from - the two-directions rule as a check.
 *
 * Implementation-Generics.md §10 asks for it as a *behaviour* - "compiling with it forced to always
 * generic and always specialize produces the same observable behavior" - and that fixture needs the
 * erased form, so it is G5's. What is checkable with one form is the half underneath it: **a
 * specialization is the generic read under a substitution and differs in nothing else.** Adding or
 * removing one may not change which instance was selected, which conventions were chosen or when a
 * value is dropped, and that claim rests entirely on the signature being the substituted signature
 * and the body being the same body.
 *
 * It was a hope and not a check. `verifyProgram` skipped every specialization on the grounds that
 * `specializeFunction` is the only thing that can make one, which is true and is not the same
 * statement: it says nothing about whether that one producer got every field right. This session
 * produced a counterexample - a witness lookup "specialized" a callee that binds nothing, giving a
 * declaration with `specializes` set and an empty argument list, which nothing reported until an
 * assertion fired several passes later inside `substitutionOf`. The first two checks below are that
 * bug.
 *
 * Run **after** the summaries rather than beside the body verifier, which is the whole reason it is
 * a pass of its own: specializations are made by the reachability walk and by the analysis, so at
 * `verifyProgram` time there are none and a check there would have been a loop over nothing.
 *
 * Reported as an internal error, because every one of these is the compiler disagreeing with itself.
 */
static bool checkSpecialization(Context& context, Program& program, Function& clone) {
    auto base = *program.decl;
    auto& generic = *base[clone.specializes];

    auto fail = [&](StringView what) {
        context.diagnostics.error("internal: the specialization %@ of %@ %@"_v, clone.source,
                                  context.findName(clone.symbol), context.findName(generic.name),
                                  what);
        return false;
    };

    // A declaration that binds nothing has nothing to be a specialization *at*, and an argument list
    // of the wrong length is a substitution that would read past its own arguments.
    if(!isGeneric(base, generic.gen)) return fail("stands at a declaration that binds nothing"_v);

    if(clone.typeArguments.size() != base[generic.gen]->variables.size()) {
        return fail("has an argument list its generic's parameters do not match"_v);
    }

    // A specialization is what a *bound* signature is. One still holding a variable would be a
    // second generic under another name, with an environment nothing fills and a layout nothing has.
    for(auto argument: clone.typeArguments.contents(base)) {
        if(!isConcreteType(base, argument)) return fail("was made at a type that is not concrete"_v);
    }

    /*
     * The body, by handle. Not "the same nodes" checked node by node - the point of the
     * representation is that there is one body and a specialization does not have one of its own, so
     * the check is that the handles are equal and there is nothing else to compare.
     */
    if(clone.mir != generic.mir) return fail("does not share its generic's body"_v);

    /*
     * The signature, position by position, against `substituteOccurrence` of the generic's.
     *
     * The *whole* occurrence and not the type: the contract and the interior shape are what say
     * whether a parameter is borrowed or owned and how it manifests, and substituting a concrete
     * type for `a` may not change either. "Substituting `Int` for unconstrained `a` does not turn a
     * borrow into a copy" is §10's own sentence, and this is where it stops being one.
     */
    Substitution with = substitutionOf(base, clone);

    if(clone.args.size() != generic.args.size()) {
        return fail("takes a different number of arguments than its generic"_v);
    }

    for(U32 i = 0; i < clone.args.size(); i++) {
        auto& declared = *base[generic.args.get(base, i)];
        auto& bound = *base[clone.args.get(base, i)];

        auto wanted = substituteOccurrence(context, program, declared.occurrence, with);
        if(bound.occurrence != wanted) {
            return fail("has a parameter that is not its generic's substituted"_v);
        }
    }

    if(clone.result != substituteOccurrence(context, program, generic.result, with)) {
        return fail("has a result that is not its generic's substituted"_v);
    }

    /*
     * Everything else a `Function` says about itself, which a substitution may not touch.
     *
     * `teardown` and `lens` are properties of the *declaration* - what it is for and how it is
     * called - `originSlots` is a count of what its signature publishes, and the module is where the
     * body is emitted. A clone that disagreed on any of them would be a different function wearing
     * the generic's body, which is exactly the failure this check exists to name.
     */
    if(clone.teardown != generic.teardown || clone.lens != generic.lens
       || clone.originSlots != generic.originSlots || clone.module != generic.module) {
        return fail("disagrees with its generic about what kind of declaration it is"_v);
    }

    /*
     * A specialization is **never itself a class member**, and this is the one field where the two
     * deliberately differ rather than agree.
     *
     * A class *default* is a generic body over the class's own context, so what runs for one
     * instance is that default specialized at the class's arguments - and its generic is the class's
     * declaration of the member, which carries `klass`. The clone may not: `callTarget` recognizes a
     * class member by that field and turns the call into an instance selection, so a specialization
     * that kept it would be dispatched a second time at the answer dispatch had already reached.
     *
     * Found by this check on `Classes.yana`'s `twice`, which is the case worth having: the field is
     * cleared by `specializeFunction` never setting it, and "never set" and "must not be set" read
     * the same in the code and differently the first time someone adds a line.
     */
    if(clone.klass) return fail("is a class member, which dispatch has already answered"_v);

    return true;
}

bool checkSpecializations(Context& context, Program& program) {
    auto base = *program.decl;
    auto valid = true;

    for(auto modulePointer: program.modules) {
        for(auto functionPointer: base[modulePointer]->functions.contents(base)) {
            auto& function = *base[functionPointer];
            if(!function.specializes) continue;

            valid &= checkSpecialization(context, program, function);
        }
    }

    return valid;
}

bool verifyProgram(Context& context, Program& program) {
    auto declBase = *program.decl;
    auto valid = true;

    for(auto modulePointer: program.modules) {
        auto module = declBase[modulePointer];
        for(auto functionPointer: module->functions.contents(declBase)) {
            /*
             * A specialization shares the generic's body, so verifying it would be verifying the
             * same nodes a second time under a substitution the check does not need: every rule here
             * is about the body as the resolver built it, and that is what the generic is.
             *
             * The half that *is* specific to a specialization - that its signature is the generic's
             * substituted - is `checkSpecializations`, which runs after the walks that make one.
             */
            auto& function = *declBase[functionPointer];
            if(function.specializes) continue;

            valid &= verifyBody(context, program, function);
        }
    }

    return valid;
}

} // namespace mir
