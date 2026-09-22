#include "convert_internal.h"

/*
 * A **constant** as JSIR - §2.4's "one constant lowering, and it is this directory's".
 *
 * A global's initializer is the one value on this target that no body builds, and
 * `codegen/js/assemble.cpp` used to write it with a second walk over `mir::` types. That copy had
 * no niche arm and no cycle rule, so a `Maybe(I64)` global emitted a `number` Node refuses to mix
 * with a `bigint` and a recursive `data` exhausted the arena (J79 0.1). `runConstant` builds a
 * throwaway body instead and `js::structureConstant` spells it, so the record a global builds and
 * the record a body builds go through the same arms.
 *
 * `constantLiteral` is the other half and is the scalar case: what a `mir::Constant` is as one
 * `JsConstant`, where the representation has a literal form at all.
 */

namespace jsir {

/*
 * One module-level constant as a body of its own - one block, the constant, and a `return` of it.
 *
 * The **only** constant lowering on this target since 2026-09-22. `assemble.cpp` used to carry a
 * second one, walking `mir::` types to produce AST directly, and two walks over one decision is the
 * shape §2.4 forbids: that one had no niche arm at all, so a global `Just(3000000000000)` at a
 * `Maybe(I64)` came out as the *number* three trillion and Node refused to mix it with a `bigint`;
 * and it closed a recursive record with a depth cap where `zeroOf` closes it with a cycle set, so
 * `data Branch = Leaf | Node {left: Branch, right: Branch}` built an exponential tree of zeros and
 * exhausted the arena before the cap fired.
 *
 * A body rather than a shared expression builder because the *decision* is what has to be shared and
 * the two outputs genuinely differ: this produces JSIR and the file wants syntax. So the file asks
 * for a body and spells it - `structureConstant` in `codegen/js/structure.cpp` - through the same
 * arms every instruction in every function goes through, which is what makes "the two agree about a
 * record's properties" true by construction rather than by inspection.
 *
 * `boxed` is whether this global is one of `planBoxedGlobals`', and the `{$v}` object goes in here
 * rather than around the file's expression for the same reason: a box is a shape, and a shape is a
 * representation decision.
 */
bool Conversion::runConstant(const mir::Constant* constant, JsRepId rep, bool boxed,
                             LocationId where) {
    auto entry = builder.addBlock(StringId());
    body.entry = entry - base();
    builder.switchTo(entry);

    // A binding holds one host value, so a global of vector type holds the lane row - the same
    // `jsStoredRep` every place is read at, applied to the one storage that is not a place.
    auto stored = jsStoredRep(target, rep);
    auto value = constant ? constantOf(*constant, stored, where) : zeroOf(stored, where);

    /*
     * A constant with no value at all - `{}`, and a representation with no zero.
     *
     * `null` rather than a returned nothing: the binding exists and something reads it, and the
     * host's own absent value is the honest content. The `Void` case is what reaches it.
     */
    if(!value && !failed) {
        JsConstant absent;
        absent.kind = JsConstant::Null;
        value = builder.constant(absent, stored, where);
    }

    if(value && boxed) {
        auto rep = jsBoxRep(target, stored);
        auto inst = builder.add<JsInstObject>(rep, where, target.rep(rep).shape);
        inst->values.push(target.arena, value - base());
        value->uses++;
        value = base()[inst->result];
    }

    auto ret = builder.add<JsInstReturn>(JsRepId(), where);
    if(value) {
        ret->value = value - base();
        value->uses++;
    }

    return !failed;
}

/*
 * One entry's worth of a `Map` arm - a source constant as a literal, or nothing.
 *
 * Out of line because `Conversion` declares it before `constantOf` is defined; it is the same walk
 * with the instruction stripped off, since a map's arms are written into the emitted chain rather
 * than computed.
 */
Maybe<JsConstant> Conversion::constantLiteral(const mir::Constant& constant, JsRepId repId) {
    /*
     * A **niche-folded** answer is a literal too: `Nothing` is `null` and `Just(k)` is `k`'s own
     * literal - `aggregateConstant`'s niche arm, with nothing built. Without it a `match` answering
     * `Maybe(I32)` was a `Select` chain over the same literals (J88), which spells the same ternary
     * but never reaches `mapExpr`'s tables.
     */
    if(auto niche = nicheOf(repId)) {
        if(!constant.type || decl[constant.type]->kind != mir::Type::Record) return Nothing();

        U16 constructor = 0;
        const mir::Constant* children = nullptr;
        U32 childCount = 0;

        if(constant.kind == mir::Constant::Enum) {
            constructor = constant.asConstructor();
        } else if(constant.kind == mir::Constant::Aggregate) {
            auto aggregate = decl[constant.aggregate()];
            constructor = aggregate->constructor;
            children = aggregate->values.pointer(decl);
            childCount = U32(aggregate->values.size());
        } else {
            return Nothing();
        }

        auto arm = ((mir::RecordType*)decl[constant.type])->cons.get(decl, constructor);
        if(I32(arm.value) != niche->presentTag) {
            JsConstant absent;
            absent.kind = JsConstant::Null;
            return Just(absent);
        }

        auto at = U32(niche->payloadField - arm.first);
        if(at >= childCount) return Nothing();

        auto content = niche->content;
        return constantLiteral(children[at], content);
    }

    auto& rep = target.rep(repId);

    switch(constant.kind) {
        case mir::Constant::Int: {
            auto bits = constant.asInt();

            JsConstant literal;
            if(rep.kind == JsRepKind::BigInt) {
                literal.kind = JsConstant::BigInt;
                literal.bits = bits;
                literal.isSigned = rep.domain == NumberDomain::Signed;
                return Just(literal);
            }

            if(rep.kind != JsRepKind::Number) return Nothing();

            literal.kind = JsConstant::Number;
            literal.number = rep.domain == NumberDomain::Unsigned && rep.bits < 64
                ? F64(bits & ((U64(1) << rep.bits) - 1)) : F64(I64(bits));
            return Just(literal);
        }

        case mir::Constant::Float: {
            JsConstant literal;
            literal.kind = JsConstant::Number;
            literal.number = constant.asFloat();
            literal.integral = false;
            return Just(literal);
        }

        case mir::Constant::Enum: {
            if(rep.kind != JsRepKind::Number) return Nothing();

            auto record = (mir::RecordType*)decl[constant.type];
            JsConstant literal;
            literal.kind = JsConstant::Number;
            literal.number = F64(record->cons.get(decl, constant.asConstructor()).value);
            return Just(literal);
        }

        case mir::Constant::Text: {
            JsConstant literal;
            literal.kind = JsConstant::Text;
            literal.text = constant.asText();
            return Just(literal);
        }

        default:
            return Nothing();
    }
}

} // namespace jsir
