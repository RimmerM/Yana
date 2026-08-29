#include "convert.h"

namespace ssa {

namespace {

/*
 * The three dense side tables a conversion needs, kept across functions.
 *
 * Cleared and refilled per body rather than constructed per body, which is the containers guidance's
 * rule: a conversion that built three fresh `Array`s per function paid a heap allocation per table
 * per function for tables whose peak size is the largest body in the program. One instance holds the
 * high-water mark and every function after the first reuses it.
 */
struct Scratch {
    // MIR node ID to the SSA value it produced. Dense, because a node ID is dense - §8.6's rule
    // about side tables rather than fields, applied to the tables a conversion needs.
    Array<SsaPtr<Value>> produced;
    Array<SsaPtr<Block>> blocks;

    // The storage each MIR local became, by local index. One `Alloc` in the entry block per local,
    // made before any block is converted, so that a read in the loop body and the write that
    // precedes it name the same address without either of them deciding where it came from.
    Array<SsaPtr<Value>> slots;

    void reset(U32 nodes) {
        produced.clear();
        blocks.clear();
        slots.clear();

        produced.reserve(nodes);
        for(U32 i = 0; i < nodes; i++) produced.push(nullptr);
    }
};

struct Conversion {
    Context& context;
    mir::Program& program;
    SsaArena& arena;
    mir::Function& function;
    mir::Body& source;
    MirBase mir;
    Scratch& scratch;

    Editor editor;

    Array<SsaPtr<Value>>& produced;
    Array<SsaPtr<Block>>& blocks;
    Array<SsaPtr<Value>>& slots;

    U32 errors = 0;

    Conversion(Context& context, mir::Program& program, SsaArena& arena, mir::Function& function,
               Scratch& scratch):
        context(context), program(program), arena(arena), function(function),
        source(*(*program.mir)[function.mir]), mir(*program.mir), scratch(scratch),
        editor(context, program, arena, function),
        produced(scratch.produced), blocks(scratch.blocks), slots(scratch.slots)
    {
        scratch.reset(source.nodes.size());
    }

    SsaBase base() { return *arena; }
    DeclBase decl() { return *program.decl; }

    void report(LocationId at, StringView text) {
        errors++;
        context.diagnostics.error(text, at);
    }

    /*
     * What an MIR occurrence becomes at this tier.
     *
     * The mapping is here and nowhere else. An owned occurrence of a type with no teardown is
     * `Trivial` rather than `Owned`: the linear invariant exists to make sure a teardown runs
     * exactly once, and a value with none has nothing for it to be true of. A borrowed occurrence is
     * `Guaranteed` whatever its type, because the fact that has to survive is the dependency on the
     * loan rather than anything about the representation.
     */
    Convention conventionFor(const mir::Occurrence& occurrence) {
        if(!occurrence.contract.isOwned()) return Convention::Guaranteed;
        return mir::hasTeardown(decl(), occurrence.type) ? Convention::Owned : Convention::Trivial;
    }

    // The use an MIR mode becomes. `Forward` must not reach here: liveness resolves it to `Take`
    // or reports that the source needs an explicit spelling, and a `Forward` in a converted body is
    // that pass having been skipped.
    bool useKindFor(mir::UseMode mode, UseKind& into) {
        switch(mode) {
            case mir::UseMode::Take:
            case mir::UseMode::CopyTrivial:
                into = UseKind::Consume;
                return true;
            case mir::UseMode::BorrowShared:
            case mir::UseMode::BorrowUnique:
                into = UseKind::Borrow;
                return true;
            case mir::UseMode::Inspect:
                into = UseKind::Inspect;
                return true;
            case mir::UseMode::Forward:
            case mir::UseMode::Count:
                return false;
        }

        return false;
    }

    /*
     * The address of a place.
     *
     * A place is a root and a projection path, and this is the walk that turns it into arithmetic.
     * Only the two forms this milestone builds; a projection becomes a field address and an index a
     * computed one, and both belong here rather than at each consumer, which is the single-place rule
     * the whole rewrite is organized around.
     */
    Value* addressOf(mir::Place& place, LocationId at) {
        switch(place.kind) {
            case mir::Place::Global: {
                auto global = DeclPtr<mir::Global>(place.payload);
                return editor.resultOf(*editor.addGlobalAddress(at, global));
            }
            case mir::Place::Local: {
                if(place.payload >= slots.size() || !slots[place.payload]) {
                    report(at, "internal: a place names a local with no storage"_v);
                    return nullptr;
                }

                return base()[slots[place.payload]];
            }
            case mir::Place::Loan:
            case mir::Place::Address:
            case mir::Place::Field:
            case mir::Place::Index:
            case mir::Place::Deref:
            case mir::Place::KindCount:
                break;
        }

        report(at, "internal: the ownership pipeline cannot lower this place yet"_v);
        return nullptr;
    }

    Value* valueOf(mir::Operand operand, LocationId at) {
        switch(operand.source()) {
            case mir::SourceKind::Result: {
                auto value = produced[operand.nodeId().index];
                if(!value) {
                    report(at, "internal: an operand names a node that produced no value"_v);
                    return nullptr;
                }

                return base()[value];
            }
            case mir::SourceKind::Constant: {
                // Copied out of the body's table by value, which is what lets the MIR region be
                // released the moment this walk finishes - see `InstConst`.
                auto constant = source.constant(mir, operand.constantId());
                return editor.resultOf(*editor.addConst(at, constant));
            }
            case mir::SourceKind::Place:
                report(at, "internal: a place reached a position that wants a value"_v);
                return nullptr;
        }

        return nullptr;
    }

    /*
     * One MIR operand as the SSA use it becomes: the value, and what is done to it.
     *
     * The pair together, because both halves fail for reasons the caller has to report at the same
     * place - a `Forward` that liveness never resolved, and an operand naming a node that produced
     * nothing. Returning false means a diagnostic has already been made.
     */
    bool useOf(mir::Operand operand, LocationId at, Use& into) {
        UseKind kind;
        if(!useKindFor(operand.mode(), kind)) {
            report(at, "internal: an unresolved forward reached SSA conversion"_v);
            return false;
        }

        auto value = valueOf(operand, at);
        if(!value) return false;

        into = editor.use(*value, kind);
        return true;
    }

    void convertNode(mir::Node& node) {
        switch(node.kind) {
            case mir::Node::Const: {
                auto& written = (mir::NodeConst&)node;
                auto constant = source.constant(mir, written.constant);
                produced[node.id.index] =
                    editor.resultOf(*editor.addConst(node.source, constant)) - base();
                break;
            }
            case mir::Node::Read: {
                auto& written = (mir::NodeRead&)node;
                auto place = source.place(mir, written.place.placeId());
                auto address = addressOf(*place, node.source);
                if(!address) return;

                /*
                 * The load, and the convention its result carries.
                 *
                 * Taken from the *read's own occurrence*, which the MIR builder decided from the
                 * mode - so a trivially copied scalar is `Trivial`, a taken owner is `Owned` and a
                 * borrow is `Guaranteed`, and this stage neither re-derives that nor is entitled to a
                 * second opinion about it.
                 */
                auto& result = mir::valueOccurrence(node);
                auto load = editor.addLoad(node.source, editor.use(*address, UseKind::Inspect),
                                           result, conventionFor(result));

                produced[node.id.index] = editor.resultOf(*load) - base();
                break;
            }
            case mir::Node::Argument:
                // Made when its block was, before anything could name it - see `run`. Nothing to do
                // here, and nothing to skip either: the entry is already filled.
                break;
            case mir::Node::Binary: {
                auto& written = (mir::NodeBinary&)node;
                Use lhs, rhs;
                if(!useOf(written.lhs, node.source, lhs)) return;
                if(!useOf(written.rhs, node.source, rhs)) return;

                auto& result = mir::valueOccurrence(node);
                auto inst = editor.addBinary(node.source, written.op, lhs, rhs, result,
                                             conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Unary: {
                auto& written = (mir::NodeUnary&)node;
                Use value;
                if(!useOf(written.value, node.source, value)) return;

                auto& result = mir::valueOccurrence(node);
                auto inst = editor.addUnary(node.source, written.op, value, result,
                                            conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Write: {
                auto& written = (mir::NodeWrite&)node;
                auto place = source.place(mir, written.destination.placeId());
                auto address = addressOf(*place, node.source);
                if(!address) return;

                /*
                 * What was there before, and why nothing is dropped here yet.
                 *
                 * `Destination::Assign` over a type with a teardown is where the old contents have
                 * to be released before the new ones land, and `Reinit` is where a drop flag decides
                 * whether they do. Every type at this milestone is trivial, so the placement is
                 * stated and not reached; the first fixture with an owner is what exercises it.
                 */
                Use value;
                if(!useOf(written.value, node.source, value)) return;

                editor.addStore(node.source, editor.use(*address, UseKind::Inspect), value);
                break;
            }
            case mir::Node::Return: {
                auto& written = (mir::NodeReturn&)node;
                if(!written.hasValue) {
                    editor.addReturn(node.source, nullptr, 0);
                    break;
                }

                Use used;
                if(!useOf(written.value, node.source, used)) return;

                editor.addReturn(node.source, &used, 1);
                break;
            }
            case mir::Node::Jump: {
                auto& written = (mir::NodeJump&)node;
                auto target = base()[blocks[mir[written.to.target]->index.index]];

                SmallArray<Use, 8> arguments;
                for(auto operand: written.to.arguments.contents(mir)) {
                    Use used;
                    if(!useOf(operand, node.source, used)) return;
                    arguments.push(used);
                }

                editor.addJump(node.source, *target, arguments.pointer(), arguments.size());
                break;
            }
            case mir::Node::Branch: {
                auto& written = (mir::NodeBranch&)node;
                auto value = valueOf(written.condition, node.source);
                if(!value) return;

                editor.addBranch(node.source, editor.use(*value, UseKind::Inspect),
                                 *base()[blocks[mir[written.whenTrue.target]->index.index]],
                                 *base()[blocks[mir[written.whenFalse.target]->index.index]]);
                break;
            }
            case mir::Node::Unreachable:
                editor.addUnreachable(node.source);
                break;
            case mir::Node::KindCount:
                break;
        }
    }

    bool run() {
        /*
         * Blocks first, then contents.
         *
         * A terminator names a successor that may not have been converted yet, so the block map has
         * to be complete before any instruction is built. The entry block already exists - the
         * editor made it - so it is mapped rather than created.
         */
        blocks.reserve(source.blocks.size());
        blocks.push(editor.body.entry);

        for(U32 i = 1; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
            blocks.push(editor.addBlock(block->name) - base());
        }

        /*
         * The locals, as storage in the entry block.
         *
         * Before any block is converted, because a loop body reads a local the header wrote and
         * neither of them is entitled to decide where it lives. The entry is where they go for the
         * ordinary reason a frame slot does: it dominates everything, so no path reaches a read
         * whose storage has not been established.
         */
        editor.setBlock(*base()[blocks[0]]);
        slots.reserve(source.locals.size());

        for(U32 i = 0; i < source.locals.size(); i++) {
            auto local = source.local(mir, LocalId(i));

            // The placement is the local's, decided by MIR ownership analysis - see
            // `mir::StorageClass`. This walk carries it down and does not choose one.
            auto alloc = editor.addAlloc(local->source, local->occurrence, local->storage);
            slots.push(editor.resultOf(*alloc) - base());
        }

        for(U32 i = 0; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
            auto target = base()[blocks[i]];

            for(auto argumentPointer: block->arguments.contents(mir)) {
                auto& argument = *mir[argumentPointer];
                auto& occurrence = mir::valueOccurrence(argument);
                auto value = editor.addBlockArgument(*target, occurrence,
                                                     conventionFor(occurrence));

                // The parameter is a node in MIR and a value here, so the map from one to the other
                // is the same `produced` table every other result goes through - which is what lets
                // an operand naming a parameter need no case of its own.
                produced[argument.id.index] = value - base();
            }
        }

        for(U32 i = 0; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
            editor.setBlock(*base()[blocks[i]]);

            for(auto nodePointer: block->nodes.contents(mir)) convertNode(*mir[nodePointer]);
        }

        editor.seal();
        return errors == 0;
    }
};

} // namespace

bool convertProgram(Context& context, mir::Program& program, Converted& into) {
    auto decl = *program.decl;
    auto converted = true;

    // One set of side tables for the whole program, reset per body - see `Scratch`.
    Scratch scratch;

    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];

        for(auto functionPointer: module->functions.contents(decl)) {
            auto function = decl[functionPointer];
            if(!function->reachable || !function->mir) continue;

            Conversion conversion(context, program, into.arena, *function, scratch);
            converted &= conversion.run();
        }
    }

    return converted;
}

} // namespace ssa
