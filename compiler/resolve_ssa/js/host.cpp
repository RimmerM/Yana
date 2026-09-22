#include "convert_internal.h"

/*
 * The host rows - `mir::HostOp`, `resolve_mir/host.def` and Analysis-IR-JS.md §3.10.
 *
 * Almost all of it is a pass-through: the row, the member name and the operands go onto a
 * `JsInstHost` and `codegen/js/structure.cpp` decides what each one is spelled as. Two things are
 * decided here because they are not spellings.
 *
 * **The three vector transfers become the run of element accesses they are.** A row rather than
 * `lanes` places written out at expansion is `op.def`'s decision, and this is where the node
 * becomes the run - straight-line by construction, since a vector is `lanes` values.
 *
 * **`.length` carries the host's own range.** See `JsInstHost::hostRange`, which is where the one
 * row that has one is argued; it was the AST tier matching the member name until 2026-09-22 (J80).
 */

namespace jsir {

void Conversion::convertHost(ssa::InstHost& host) {
    auto where = host.source;

    auto repId = host.results.size() ? repOf(*ssa[host.results.get(ssa, 0)]) : jsVoidRep(target);

    /*
     * `.length` is the one range the **host** specifies rather than a Yana type - `hostRange`.
     *
     * Decided here rather than at the AST tier, which is where it was until 2026-09-22 (J80):
     * `codegen/js/structure.cpp` matched the member name to claim it, in the one file whose rule is
     * that it switches on nothing but a `JsRep`, a `JsShape` and a `JsPlace`, and the shipping
     * backend's `inst.cpp` said the same sentence beside it. What it is *not* is a representation:
     * `hostStringLength` is declared `-> Size`, so labelling the result unsigned makes the `return`
     * and every edge carrying it a value the verifier is right to refuse. See `JsInstHost::hostRange`.
     */
    auto hostRange = host.op == mir::HostOp::Field && host.results.size() &&
                     stringView(target.context.findName(host.member)) == "length"_v;

    /*
     * The three vector transfers - `lanes` element accesses each, and no loop.
     *
     * A row here rather than `lanes` places written out at expansion is `op.def`'s decision and the
     * reason it gives is this tier's gain: a run of `lanes` reads is `lanes` bounds questions for
     * every pass above to carry where one node is one. This is where the node becomes the run, and
     * it is straight-line by construction - a vector is `lanes` values, so writing the lanes out one
     * at a time is not an unrolling of anything.
     *
     * `hostVectorUpTo` is the **tail**, and clamping is the emitter's because the index it clamps is
     * the lane's rather than anything the source wrote: `min(index + i, limit)`, so a lane past the
     * end reads the last element instead of `undefined`.
     */
    if(host.op == mir::HostOp::Vector || host.op == mir::HostOp::VectorUpTo ||
       host.op == mir::HostOp::SetVector) {
        auto setting = host.op == mir::HostOp::SetVector;
        auto clamped = host.op == mir::HostOp::VectorUpTo;
        auto count = host.arguments.size();

        if(count < (clamped || setting ? 3u : 2u)) {
            internal("met a host vector transfer with too few operands"_v, where);
            return;
        }

        auto row = valueOf(host.arguments.get(ssa, 0).value(), where);
        auto at = valueOf(host.arguments.get(ssa, 1).value(), where);
        if(!row || !at) {
            internal("met a host vector transfer with no row or index"_v, where);
            return;
        }

        auto vectorRep = lanesRepOf(*ssa[setting ? host.arguments.get(ssa, 2).value()
                                                  : host.results.get(ssa, 0)]);
        if(!vectorRep.valid() || target.rep(vectorRep).kind != JsRepKind::Parts) {
            report("a host vector transfer of something that is not a vector"_v, where);
            return;
        }

        auto lanes = target.rep(vectorRep).partCount;
        auto lane = partRepAt(target, vectorRep, 0);
        auto indexRep = at->rep;

        JsValue* limit = nullptr;
        if(clamped) {
            limit = valueOf(host.arguments.get(ssa, 2).value(), where);
            if(!limit) {
                internal("met a clamped host vector transfer with no limit"_v, where);
                return;
            }
        }

        auto indexAt = [&](U16 i) -> JsValue* {
            auto index = at;
            if(i) {
                index = binaryOf(mir::BinaryOp::Add, indexRep, at,
                                 builder.number(F64(i), indexRep, where), where);
            }

            if(!limit) return index;

            auto over = binaryOf(mir::BinaryOp::Gt, jsNumberRep(target, NumberDomain::Bool01, 1),
                                 index, limit, where);
            return selectOf(over, limit, index, indexRep, where);
        };

        if(setting) {
            auto value = requireParts(host.arguments.get(ssa, 2).value(), vectorRep, where);
            if(!value.valid()) {
                internal("met a host vector store with no parts"_v, where);
                return;
            }

            for(U16 i = 0; i < lanes; i++) {
                auto index = indexAt(i);
                if(!index) return;

                builder.store(builder.addElement(row, index, lane), partAt(value, i), where);
            }

            if(host.results.size()) setVoid(ssa[host.results.get(ssa, 0)]->id.index);
            return;
        }

        SmallArray<JsValue*, 16> loaded;
        for(U16 i = 0; i < lanes; i++) {
            auto index = indexAt(i);
            if(!index) return;

            loaded.push(builder.load(builder.addElement(row, index, lane), where));
        }

        defineParts(host, makeParts(vectorRep, held(loaded)));
        return;
    }

    // The operands before the node, for `collectEdge`'s reason - reading a place may emit a load,
    // and a node built first would have its own operands computed after it.
    SmallArray<JsValue*, 8> arguments;
    for(auto used: host.arguments.contents(ssa)) {
        auto value = valueOf(used.value(), where);
        if(!value) {
            internal("met a host operand with no value"_v, where);
            return;
        }

        arguments.push(value);
    }

    auto inst = builder.add<JsInstHost>(isVoidRep(repId) ? JsRepId() : repId, where, host.op,
                                        host.member);
    inst->hostRange = hostRange;

    for(auto value: arguments) {
        inst->arguments.push(target.arena, value - base());
        value->uses++;
    }

    if(host.results.size()) {
        auto result = ssa[host.results.get(ssa, 0)];
        if(isVoidRep(repId)) setVoid(result->id.index);
        else setOne(result->id.index, base()[inst->result] - base());
    }
}

} // namespace jsir
