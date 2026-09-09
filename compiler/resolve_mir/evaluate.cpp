#include "resolve_internal.h"
#include "fold.h"

namespace mir {

/*
 * Running a function at compile time, over constant arguments - `Resolver::evaluateCall`.
 *
 * ## Why there is an interpreter here at all
 *
 * A string literal is `Constant::Text`, which says what the value *is* and leaves how it is built
 * to the library: `stringLiteral` takes the address of the bytes and a count and describes a run
 * that owns nothing. In a *value* position `textValue` calls it and the answer is instructions. In
 * a **global's initializer** there are no instructions - the value is bytes written before anything
 * runs - so something has to produce what that call would have produced, without a call.
 *
 * The alternative was to spell the fields of `String` here, which is exactly what
 * `resolve/const.cpp` does in the shipping stage and why it has a "this target's string layout is
 * not the one this compiler writes" diagnostic to report. Running the library's own function is the
 * same answer with no second description of a type the library owns: what changes when the library
 * changes is the library.
 *
 * ## Why MIR rather than the AST
 *
 * The body has already been through name resolution, overload selection, generic substitution and
 * `expandIntrinsic`, so there is nothing here about scopes, instances or conversions. What is left
 * is nine node kinds over a straight line, and `truncate(length) :: Count` has already become the
 * `convert` it is. An evaluator over the written form would have had to re-decide every one of
 * those, which is the second answer this stage exists to avoid.
 *
 * ## What it will and will not run
 *
 * A body of one block ending in a `return`, whose nodes are constants, reads of places that hold
 * constants, conversions, aggregates and calls of functions this same rule accepts. Anything else -
 * a branch, a loop, a store, an allocation, arithmetic - is **not a constant** rather than a
 * diagnostic: the answer is unset and the caller says what the position needed.
 *
 * That is deliberately narrower than "a pure function". Widening it is a matter of adding node
 * kinds here - `Binary` folded would make `4 * 1024 * 1024` a constant, which
 * `Analysis-Initialization.md`'s stage A names as the one thing left of it - and each of those is a
 * decision about the *language* rather than about this file. What is here is what the library's
 * string builder needs, stated so that the next one is an arm and not a mechanism.
 */
struct ConstantRun {
    Resolver& resolver;
    DeclBase decl;
    MirBase mir;

    // How deep the nesting of calls is, against `kDepthLimit`. A body that calls itself with the
    // same arguments would not terminate, and this is a compiler that has to answer rather than
    // hang; nothing in the corpus goes past one level.
    static constexpr U32 kDepthLimit = 8;
    U32 depth = 0;

    /*
     * One activation - the body being run, and what its nodes and locals hold.
     *
     * By index into the body's own dense tables, which is what those tables are for: a node's ID is
     * its position, so a scratch array is the whole of the state an interpreter needs. `Constant`'s
     * unset value doubles as "not computed", which is sound because a node that produced no
     * constant is one this run has already given up on.
     */
    struct Frame {
        Body& body;
        Array<Constant> nodes;
        Array<Constant> locals;
    };

    /*
     * How many blocks one run may enter, against a body whose control flow it follows.
     *
     * A cap rather than a visited set, because the shape being refused is a *loop*: a body whose
     * back edge is taken on a constant condition would run forever, and one that terminates does so
     * in a bounded number of steps this number is generous about. A teardown is two or three.
     */
    static constexpr U32 kBlockLimit = 32;

    Constant run(Function& callee, const Constant* arguments, Size count);
    mir::Block* takeEdge(Frame& frame, Edge& edge);
    Constant evaluate(Frame& frame, Node& node);
    Constant valueOf(Frame& frame, Operand operand);
    Constant readPlace(Frame& frame, PlaceId id);
    Constant fieldOf(const Constant& parent, U32 index);
};

/*
 * One operand's value - the same three sources every operand has.
 *
 * A `Place` operand is read rather than refused, because a read *is* how a body names a parameter
 * and a global: `NodeRead::place` is the only operand in the accepted set that names one.
 */
Constant ConstantRun::valueOf(Frame& frame, Operand operand) {
    switch(operand.source()) {
        case SourceKind::Constant:
            return frame.body.constant(mir, operand.constantId());
        case SourceKind::Result:
            return frame.nodes[operand.nodeId().index];
        case SourceKind::Place:
            return readPlace(frame, operand.placeId());
    }

    return Constant();
}

// A field of an aggregate constant - `mir::constantField`, beside the kind, because the same walk
// answers a read of a slot the optimizer found filled by one.
Constant ConstantRun::fieldOf(const Constant& parent, U32 index) {
    return constantField(decl, parent, index);
}

/*
 * What is at a place, where that is a constant.
 *
 * Three roots and one projection. A **local** holds what an argument or an earlier write put there;
 * a **global** holds its own initializer, which is what makes `runBorrowed` readable from inside
 * `stringLiteral` and is the "reference to an earlier immutable global"
 * `Analysis-Initialization.md`'s stage A leaves open in the *written* language. A loan and a raw
 * address are roots this cannot follow at all - there is no storage yet for either to name.
 */
Constant ConstantRun::readPlace(Frame& frame, PlaceId id) {
    auto place = frame.body.place(mir, id);

    switch(place->kind) {
        case Place::Local:
            return frame.locals[place->payload];
        case Place::Global:
            return decl[DeclPtr<Global>(place->payload)]->initial;
        case Place::Field:
            return fieldOf(readPlace(frame, mir[place->parent]->id), place->payload);
        default:
            return Constant();
    }
}

/*
 * One node, given the ones before it.
 *
 * The accepted set, and what each is. Everything absent from it answers an unset constant, which is
 * what stops the run - see the file comment for why that is the narrow answer rather than a
 * diagnostic.
 */
Constant ConstantRun::evaluate(Frame& frame, Node& node) {
    switch(node.kind) {
        case Node::Const:
            return frame.body.constant(mir, ((NodeConst&)node).constant);

        case Node::Read:
            return valueOf(frame, ((NodeRead&)node).place);

        /*
         * A conversion, which at a constant is the number in the destination's normal form.
         *
         * `foldLiteral` is the same function `FromInt` folds through, so `truncate(10) :: Count` in
         * a library's body and `10 :: Count` written in a program produce the same bits. Every other
         * unary is `fold.h`'s, shared with the written constant positions above this.
         */
        case Node::Unary: {
            auto& unary = (NodeUnary&)node;
            auto value = valueOf(frame, unary.value);
            if(!value.isSet()) return Constant();

            auto type = unary.occurrence.type;

            if(unary.op == UnaryOp::Convert) {
                if(decl[type]->kind != Type::Int && decl[type]->kind != Type::Float) {
                    return Constant();
                }

                // The two readings `foldLiteral` takes - a two's complement integer and a double -
                // out of whichever of them this constant is.
                auto bits = value.kind == Constant::Int ? value.asInt() : U64(I64(value.asFloat()));
                auto number = value.kind == Constant::Int ? double(I64(value.asInt()))
                                                          : value.asFloat();

                return foldLiteral(decl, type, bits, number);
            }

            return foldConstantUnary(resolver.program, unary.op, value, type);
        }

        // Arithmetic, over the same folder a written `4 * 1024` reaches - see `fold.h`, where the
        // reason a target-answered width folds only sometimes is stated.
        case Node::Binary: {
            auto& binary = (NodeBinary&)node;
            auto lhs = valueOf(frame, binary.lhs);
            auto rhs = valueOf(frame, binary.rhs);
            if(!lhs.isSet() || !rhs.isSet()) return Constant();

            return foldConstantBinary(resolver.program, binary.op, lhs, rhs,
                                      binary.occurrence.type);
        }

        case Node::Aggregate: {
            auto& aggregate = (NodeAggregate&)node;

            SmallArray<Constant, 8> values;
            for(auto component: aggregate.components.contents(mir)) {
                auto value = valueOf(frame, component);
                if(!value.isSet()) return Constant();

                values.push(value);
            }

            return resolver.program.aggregateConstant(aggregate.occurrence.type,
                                                      aggregate.constructor, values.pointer(),
                                                      values.size());
        }

        /*
         * A call, which is this same rule one level down - the callee has to be a plain function
         * with a body, and every argument has to have reduced.
         *
         * A generic callee is not one: what it would run is a specialization, and which one is a
         * question about the call site that this tier answers by having already made it. So the
         * node names a `Function` and that is the body run.
         */
        case Node::Call: {
            auto& call = (NodeCall&)node;
            if(!call.callee) return Constant();

            SmallArray<Constant, 8> values;
            for(auto argument: call.arguments.contents(mir)) {
                auto value = valueOf(frame, argument);
                if(!value.isSet()) return Constant();

                values.push(value);
            }

            return run(*decl[call.callee], values.pointer(), values.size());
        }

        default:
            return Constant();
    }
}

/*
 * One edge taken - the target block, with its arguments bound.
 *
 * Every operand is read *before* any of them is written, which is what a jump means: the arguments
 * are the predecessor's values, and a block that passes one of its own parameters along would
 * otherwise read the value it had just been given.
 */
mir::Block* ConstantRun::takeEdge(Frame& frame, Edge& edge) {
    if(!edge.target) return nullptr;

    SmallArray<Constant, 8> values;
    for(auto operand: edge.arguments.contents(mir)) {
        auto value = valueOf(frame, operand);
        if(!value.isSet()) return nullptr;

        values.push(value);
    }

    auto target = mir[edge.target];
    if(values.size() != target->arguments.size()) return nullptr;

    for(U32 i = 0; i < values.size(); i++) {
        auto argument = mir[target->arguments.get(mir, i)];
        frame.nodes[((NodeResult*)argument)->id.index] = values[i];
    }

    return target;
}

/*
 * One body, run.
 *
 * A straight line through the block graph, following the one edge each terminator names - and a
 * `branch` names one because its condition has to have **reduced to a constant** like everything
 * else here. That is the whole of what control flow costs this evaluator: no path is explored that
 * the arguments do not decide, so there is no merge to widen and no fixed point to reach, and a
 * body whose condition depends on something this cannot compute stops exactly where every other
 * unreducible node stops it.
 *
 * It was one block for a long time, and the sentence that used to be here - "the moment a branch is
 * followed this is a machine with a program counter" - was about a machine that *chooses*. This one
 * does not: it is constant folding with the fold applied to the terminator as well, which is the
 * same rewrite `cleanCfg` makes one tier down over IR rather than over values. What it buys is the
 * question `teardownRunsNothing` asks, where the whole of the body is a flag test.
 *
 * A body that returns **no value** answers `{}` rather than nothing, so that "it ran" and "it
 * produced a value" are two answers. Every caller that wants a value asks for one at a type.
 */
Constant ConstantRun::run(Function& callee, const Constant* arguments, Size count) {
    if(!callee.mir || depth >= kDepthLimit) return Constant();

    auto& body = *mir[callee.mir];
    if(!body.blocks.size()) return Constant();

    Frame frame { body, {}, {} };
    frame.nodes.reserve(body.nodes.size());
    for(U32 i = 0; i < body.nodes.size(); i++) frame.nodes.push(Constant());

    /*
     * The parameters, into the locals that *are* them - `Local::parameter`.
     *
     * A parameter is storage like any other local and the call established its contents, so binding
     * the argument is the whole of what entering the body means. A body whose signature and
     * argument list disagree is a caller mistake this cannot make good on, and it comes back unset.
     */
    frame.locals.reserve(body.locals.size());
    for(U32 i = 0; i < body.locals.size(); i++) frame.locals.push(Constant());

    for(U32 i = 0; i < body.locals.size(); i++) {
        auto local = body.local(mir, LocalId(i));
        if(!local->isParameter()) continue;
        if(local->parameter >= count) return Constant();

        frame.locals[i] = arguments[local->parameter];
    }

    depth++;
    auto answer = Constant();
    auto block = body.block(mir, BlockId(0));
    auto steps = U32(0);

    while(block) {
        if(++steps > kBlockLimit) return depth--, Constant();

        mir::Block* next = nullptr;

        for(auto pointer: block->nodes.contents(mir)) {
            auto& node = *mir[pointer];

            if(node.kind == Node::Return) {
                auto& written = (NodeReturn&)node;
                answer = written.hasValue ? valueOf(frame, written.value)
                                          : unitConstant(resolver.program.types.unit);
                break;
            }

            if(node.kind == Node::Jump || node.kind == Node::Break) {
                next = takeEdge(frame, ((NodeJump&)node).to);
                if(!next) return depth--, Constant();
                break;
            }

            if(node.kind == Node::Branch) {
                auto& written = (NodeBranch&)node;
                auto condition = valueOf(frame, written.condition);
                if(!condition.isSet()) return depth--, Constant();

                // The **stored** number is not what a condition is compared against here: a `Bool`
                // reduces to `Constant::Enum` naming its constructor, and `False` is index zero by
                // the declaration `known.def` binds. `Int` is admitted beside it because a
                // refinement may have folded the test to a number already.
                auto held = condition.kind == Constant::Enum ? U64(condition.asConstructor())
                          : condition.kind == Constant::Int ? condition.asInt()
                          : ~U64(0);
                if(held == ~U64(0)) return depth--, Constant();

                next = takeEdge(frame, held ? written.whenTrue : written.whenFalse);
                if(!next) return depth--, Constant();
                break;
            }

            if(!hasResult(node.kind)) return depth--, Constant();

            auto value = evaluate(frame, node);
            if(!value.isSet()) return depth--, Constant();

            frame.nodes[((NodeResult&)node).id.index] = value;
        }

        if(answer.isSet()) break;
        block = next;
    }

    depth--;
    return answer;
}

Constant Resolver::evaluateCall(Function& callee, const Constant* arguments, Size count) {
    ConstantRun run { *this, decl(), *program.mir };
    return run.run(callee, arguments, count);
}

/*
 * Whether the teardown of **this value** runs nothing - `Analysis-IR.md`'s "the literal's teardown
 * does not fold", answered here instead of by inlining and folding it at every site.
 *
 * A type that owes a teardown owes it for every value of that type: `String` has a `Reclaim`, and
 * whether that reclaim does anything is a *flag inside the value*. A string literal's run is
 * borrowed, so its reclaim is a test that fails and a branch that is not taken - which is exactly
 * what the evaluator above can now see, because it follows a branch whose condition reduced.
 *
 * **What makes "it ran here" mean "it does nothing there" is what the evaluator will run.** Every
 * node kind it accepts is pure - a constant, a read of a place holding one, a conversion, an
 * aggregate, and a call of a function this same rule accepts. A store, an allocation, a machine
 * operation and a call it cannot reduce all stop it. So a teardown that reduces is a teardown with
 * no effect, and one that does not reduce is kept whatever the reason - which is the direction this
 * has to fail in.
 *
 * The walk mirrors `Lowering::emitTeardown`: the authored half of each class is a call, a derived
 * half recurses into the members, and the two halves travel separately because an author's instance
 * replaces the walk for its own half only. It is the same shape asked of a *value* rather than
 * emitted over an address, and it is the one thing about a teardown this stage decides twice - the
 * comment there and this one are the pair.
 */
bool Resolver::teardownRunsNothing(DeclPtr<Type> type, const Constant& value, bool runDrop,
                                   bool runReclaim, LocationId source) {
    auto ownership = ownershipOf(decl(), type);

    auto authored = [&](intrinsic::Known klass, TeardownKind kind, bool run) {
        if(!run || kind != TeardownKind::Authored) return true;

        auto member = teardownMember(context, program, klass, type, source);
        if(!member) return true;

        return evaluateCall(*decl()[member], &value, 1).isSet();
    };

    if(!authored(intrinsic::Known::drop, ownership.drop, runDrop)) return false;
    if(!authored(intrinsic::Known::reclaim, ownership.reclaim, runReclaim)) return false;

    auto childDrop = runDrop && ownership.drop == TeardownKind::Derived;
    auto childReclaim = runReclaim && ownership.reclaim == TeardownKind::Derived;
    if(!childDrop && !childReclaim) return true;

    /*
     * The derived halves, into the members. A fixed array's elements are the same statement with the
     * index in place of the field, and anything else with a derived half is a shape this declines
     * rather than guesses at - a box is an allocation, and an allocation is the effect this exists
     * to find.
     */
    if(decl()[type]->kind == Type::Array) {
        auto& array = *(ArrayType*)decl()[type];
        if(!array.hasFixedLength()) return false;
        if(!hasTeardown(decl(), array.element)) return true;

        for(U64 i = 0; i < array.length; i++) {
            auto element = constantField(decl(), value, U32(i));
            if(!element.isSet()) return false;
            if(!teardownRunsNothing(array.element, element, childDrop, childReclaim, source)) {
                return false;
            }
        }

        return true;
    }

    if(decl()[type]->kind != Type::Record) return false;
    if(value.kind != Constant::Aggregate) return false;

    auto& record = *(RecordType*)decl()[type];
    auto arm = record.cons.get(decl(), decl()[value.aggregate()]->constructor);

    for(U16 i = 0; i < arm.count; i++) {
        auto index = U16(arm.first + i);
        auto field = record.field(decl(), index);

        // A field the record only reaches through an origin slot owes nothing: what is there is
        // somebody else's, which is the same reading `ownershipOfOccurrence` makes of a contract.
        if(!field.occurrence.contract.isOwned()) continue;
        if(field.boxed) return false;
        if(!hasTeardown(decl(), field.occurrence.type)) continue;

        auto held = constantField(decl(), value, index);
        if(!held.isSet()) return false;
        if(!teardownRunsNothing(field.occurrence.type, held, childDrop, childReclaim, source)) {
            return false;
        }
    }

    return true;
}

Constant Resolver::stringLiteralValue(DeclPtr<Global> bytes, U64 count, LocationId source,
                                      bool report) {
    auto declared = program.roles.function(intrinsic::Known::stringLiteral);
    if(!declared) {
        if(report) reportMissingRole(intrinsic::Known::stringLiteral, "a string literal"_v, source);
        return Constant();
    }

    auto& builder = *decl()[declared];
    if(builder.args.size() != 2) return Constant();

    auto units = decl()[builder.args.get(decl(), 0)]->occurrence.type;
    auto length = decl()[builder.args.get(decl(), 1)]->occurrence.type;

    Constant arguments[2] = {
        addressConstant(units, bytes),
        intConstant(length, count),
    };

    auto value = evaluateCall(builder, arguments, 2);
    if(value.isSet() || !report) return value;

    context.diagnostics.error(
        "this string has to be built before the program starts, and %@ - the library's own string builder - is not a body this compiler can run at compile time"_v,
        source, context.findName(builder.name));

    return Constant();
}

/*
 * `"abc"` in a **constant** position - the value the library's `stringLiteral` builds, built here.
 *
 * `textValue`'s two halves with the call run rather than emitted: the bytes are the same global and
 * the value is the same function, so a literal in a body and a literal in a global's initializer
 * are one description of what a string is. What comes back is an aggregate whose one pointer leaf
 * is `Constant::Address` of those bytes, which `materialize` writes as a relocation.
 *
 * It is deliberately not conditional on the library being the one in this repository. If
 * `stringLiteral`'s body is something this evaluator cannot run, the answer is the diagnostic
 * `stringLiteralValue` reports under `report` - which names the function rather than the literal,
 * because that is where the reason is. A *value* position asks the same function with `report`
 * clear, since it has a second answer: the call, opened by `@inline(always)`.
 */
Constant Resolver::textConstantValue(StringId written, LocationId source) {
    auto count = U64(context.findName(written).size());
    return stringLiteralValue(stringBytesGlobal(written, source), count, source, true);
}

/*
 * Every text leaf of a value, built - the deferral applied to an aggregate.
 *
 * A walk rather than a special case at the array, because a text may sit at any depth: `data Row
 * {name: String}` in a fixed array is two levels down and is the same statement. The aggregate is
 * re-interned through `aggregateConstant` where a leaf moved, which keeps `Constant`'s equality the
 * comparison of three words it is everywhere else.
 */
Constant Resolver::textsResolved(const Constant& constant, LocationId source) {
    if(constant.kind == Constant::Text) return textConstantValue(constant.asText(), source);
    if(constant.kind != Constant::Aggregate) return constant;

    auto& parts = *decl()[constant.aggregate()];
    SmallArray<Constant, 8> values;
    auto moved = false;

    for(auto value: parts.values.contents(decl())) {
        auto built = holdsText(decl(), value) ? textsResolved(value, source) : value;
        if(!built.isSet()) return Constant();

        moved = moved || !(built == value);
        values.push(built);
    }

    if(!moved) return constant;

    return program.aggregateConstant(constant.type, parts.constructor, values.pointer(),
                                     values.size());
}

} // namespace mir
