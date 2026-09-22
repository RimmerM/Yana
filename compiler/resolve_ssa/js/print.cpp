#include "print.h"

/*
 * See print.h. The dump is a golden from the tier's first fixture, so what it must not contain is
 * anything that moves for a reason the program did not: no arena offsets, no allocation order, no
 * pointer values. Every number here is a dense body-local index or a count the program states.
 */

namespace jsir {

namespace {

struct Printer {
    Printer(Net::Writer& writer, JsTarget& target):
        writer(writer), target(target), context(target.context), decl(target.decl),
        base(*target.arena) {}

    Net::Writer& writer;
    JsTarget& target;
    Context& context;
    DeclBase decl;
    JsirBase base;

    JsBody* body = nullptr;

    void text(StringView value) { writer.writeString(value); }

    void uint(U64 value) {
        writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
    }

    void sint(I64 value) {
        writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
    }

    void real(F64 value) {
        writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
    }

    void name(StringId id) {
        auto found = context.findName(id);
        writer.writeString(stringView(found));
    }

    void rep(JsRepId id) {
        if(!id.valid()) return text("<none>"_v);

        auto& value = target.rep(id);
        text("%r"_v);
        uint(id.index);
        (void)value;
    }

    // The declaration a `%rN` stands for, printed once in the header.
    void repDeclaration(JsRepId id) {
        auto& value = target.rep(id);

        text("rep %r"_v);
        uint(id.index);
        text(" = "_v);
        text(jsRepKindName(value.kind));

        switch(value.kind) {
            case JsRepKind::Number:
                switch(value.domain) {
                    case NumberDomain::Bool01:   text(" bool"_v); return;
                    case NumberDomain::Signed:   text(" signed "_v); uint(value.bits); return;
                    case NumberDomain::Unsigned: text(" unsigned "_v); uint(value.bits); return;
                    case NumberDomain::Float32:  text(" f32"_v); return;
                    case NumberDomain::Float64:  text(" f64"_v); return;
                }

                return;

            case JsRepKind::BigInt:
                text(value.domain == NumberDomain::Signed ? " signed"_v : " unsigned"_v);
                return;

            case JsRepKind::GcObject:
                text(" %s"_v);
                uint(value.shape.index);
                return;

            case JsRepKind::HostArray:
                text(" "_v);
                text(typedArrayName(value.typedArray));
                text(" of "_v);
                rep(value.content);
                return;

            case JsRepKind::Cell:
                text(" of "_v);
                rep(value.content);
                return;

            case JsRepKind::Nullable:
                text(" of "_v);
                rep(value.content);
                return;

            case JsRepKind::Parts: {
                text(" ("_v);
                for(U16 i = 0; i < value.partCount; i++) {
                    if(i) text(", "_v);
                    rep(target.partList[value.firstPart + i]);
                }

                text(")"_v);

                // The one value the parts become where a position holds one - absent where there is
                // none, which is what makes a `materialize` of such a representation unbuildable.
                if(value.content.valid()) {
                    text(" as "_v);
                    rep(value.content);
                }

                return;
            }

            default:
                return;
        }
    }

    void shapeDeclaration(JsShapeId id) {
        auto& shape = target.shape(id);

        text("shape %s"_v);
        uint(id.index);
        text(" "_v);

        switch(shape.kind) {
            case JsShapeKind::Object:  text("object"_v); break;
            case JsShapeKind::Cell:    text("cell"_v); break;
            case JsShapeKind::FunPair: text("fun"_v); break;
            case JsShapeKind::Ref:     text("ref"_v); break;
            case JsShapeKind::Host:    text("host"_v); break;
        }

        if(shape.source) {
            text(" "_v);
            auto declared = (mir::RecordType*)decl[shape.source];
            if(declared->name) name(declared->name);
            else text("<tuple>"_v);
        }

        text(" {"_v);

        for(U32 i = 0; i < shape.properties.size(); i++) {
            if(i) text(", "_v);

            auto& property = shape.properties[i];
            name(property.key);
            text(": "_v);
            rep(property.rep);

            if(property.role != JsPropertyRole::Field) {
                text(" "_v);
                text(jsPropertyRoleName(property.role));
            }

            if(property.role == JsPropertyRole::StorageUnit) {
                text(" #"_v);
                uint(property.packing);
            }
        }

        text("}\n"_v);
    }

    // One packed word: its fields in record order, each as `name @offset:width`, and `s` where the
    // field is read back sign-extended.
    void packingDeclaration(U32 index) {
        auto& row = target.packings[index];

        text("packing #"_v);
        uint(index);
        text(" "_v);
        rep(row.word);

        auto declared = row.source && decl[row.source]->kind == mir::Type::Record
                      ? (mir::RecordType*)decl[row.source] : nullptr;
        if(declared && declared->name) {
            text(" "_v);
            name(declared->name);
        }

        text(" {"_v);
        for(U32 i = 0; i < row.fields.size(); i++) {
            if(i) text(", "_v);

            auto& bits = row.fields[i];
            if(declared && bits.field < declared->fields.size() &&
               declared->fields.get(decl, bits.field).name) {
                name(declared->fields.get(decl, bits.field).name);
            } else {
                text("f"_v);
                uint(bits.field);
            }

            text(" @"_v);
            uint(bits.offset);
            text(":"_v);
            uint(bits.width);
            if(bits.isSigned) text("s"_v);
        }

        text("}\n"_v);
    }

    void valueRef(JsirPtr<JsValue> pointer) {
        if(!pointer) return text("<none>"_v);

        text("%"_v);
        uint(base[pointer]->id.index);
    }

    void constant(const JsConstant& value) {
        switch(value.kind) {
            case JsConstant::Number:
                real(value.number);
                return;
            case JsConstant::BigInt:
                if(value.isSigned) sint(I64(value.bits));
                else uint(value.bits);
                text("n"_v);
                return;
            case JsConstant::Text:
                text("\""_v);
                name(value.text);
                text("\""_v);
                return;
            case JsConstant::Null:
                text("null"_v);
                return;
            case JsConstant::Undefined:
                text("undefined"_v);
                return;
            case JsConstant::Bool:
                text(value.number != 0 ? "true"_v : "false"_v);
                return;
        }
    }

    /*
     * A place, printed as its access protocol rather than as the expression it becomes.
     *
     * §8.4 asks for "places with their access protocol", and the reason is the failure this tier
     * exists to catch: an assignment to the wrong property and a correct one have the same syntax,
     * so a dump that showed the syntax would show nothing.
     */
    void place(JsPlaceId id) {
        if(!id.valid() || !body || id.index >= body->places.size()) return text("<none>"_v);

        auto& value = *body->place(base, id);
        text(jsPlaceKindName(value.kind));

        switch(value.kind) {
            case JsPlaceKind::Var:
                text(" $"_v);
                uint(value.slot);
                break;
            case JsPlaceKind::Global:
                text(" "_v);
                if(value.global) name(decl[value.global]->name);
                break;
            case JsPlaceKind::Cell:
            case JsPlaceKind::Value:
                text(" "_v);
                valueRef(value.owner);
                break;
            case JsPlaceKind::Property:
                text(" "_v);
                valueRef(value.owner);
                text("."_v);
                if(value.shape.valid() && value.slot < target.shape(value.shape).properties.size()) {
                    name(target.shape(value.shape).properties[value.slot].key);
                }
                break;
            case JsPlaceKind::Element:
                text(" "_v);
                valueRef(value.owner);
                text("["_v);
                valueRef(value.index);
                text("]"_v);
                break;
            case JsPlaceKind::BitRange:
                text(" @"_v);
                uint(value.bitOffset);
                text(":"_v);
                uint(value.bitWidth);
                if(value.bitSigned) text("s"_v);
                if(value.bitScale) {
                    text(value.bitWhole ? " whole"_v : ""_v);
                    text(" scaled "_v);
                    valueRef(value.bitScale);
                }
                text(" of ("_v);
                place(value.word);
                text(")"_v);
                break;
        }

        text(": "_v);
        rep(value.content);
    }

    void edge(JsEdge& target_) {
        text(" -> "_v);
        if(!target_.target) return text("<none>"_v);

        text("block"_v);
        uint(base[target_.target]->index.index);

        if(!target_.arguments.size()) return;

        text("("_v);
        for(U32 i = 0; i < target_.arguments.size(); i++) {
            if(i) text(", "_v);
            valueRef(target_.arguments.get(base, i));
        }

        text(")"_v);
    }

    void operands(JsInst& inst) {
        auto first = true;
        forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
            text(first ? " "_v : ", "_v);
            first = false;
            valueRef(used);
        });
    }

    void inst(JsInst& node) {
        text("    "_v);

        if(node.result) {
            valueRef(node.result);
            text(": "_v);
            rep(base[node.result]->rep);
            text(" = "_v);
        }

        text(jsInstMnemonic(node.kind));

        switch(node.kind) {
            case JsInst::Const:
                text(" "_v);
                constant(((JsInstConst&)node).constant);
                return;

            case JsInst::FunCode:
                text(" "_v);
                name(decl[((JsInstFunCode&)node).callee]->name);
                return;

            case JsInst::Object:
                text(" %s"_v);
                uint(((JsInstObject&)node).shape.index);
                operands(node);
                return;

            case JsInst::Pack:
                text(" #"_v);
                uint(((JsInstPack&)node).packing);
                operands(node);
                return;

            case JsInst::Binary:
                text("."_v);
                text(mir::binaryOpName(((JsInstBinary&)node).op));
                operands(node);
                return;

            case JsInst::Unary:
                text("."_v);
                text(mir::unaryOpName(((JsInstUnary&)node).op));
                operands(node);
                return;

            case JsInst::Load:
                text(" "_v);
                place(((JsInstLoad&)node).place);
                return;

            case JsInst::Store:
                text(" "_v);
                place(((JsInstStore&)node).place);
                text(", "_v);
                valueRef(((JsInstStore&)node).value);
                return;

            case JsInst::Host: {
                // `hostOpName` already prints `host.field`, so the mnemonic's own `host` is dropped
                // rather than repeated - the row is the name here.
                auto& written = (JsInstHost&)node;
                text(" "_v);
                text(mir::hostOpName(written.op));
                if(written.member) {
                    text(" \""_v);
                    name(written.member);
                    text("\""_v);
                }

                // The host's own range claim, where it is narrower than the representation - one
                // row has one and the dump says which, for J79 0.3's reason: a decision no dump
                // shows is one that can stop being made with nothing to notice it.
                if(written.hostRange) text(" host-range"_v);

                operands(node);
                return;
            }

            case JsInst::Call:
                text(" "_v);
                name(decl[((JsInstCall&)node).callee]->name);
                operands(node);
                return;

            case JsInst::CallValue:
                operands(node);
                return;

            case JsInst::Helper:
                text("."_v);
                text(jsHelperOpName(((JsInstHelper&)node).op));
                operands(node);
                return;

            case JsInst::Map: {
                auto& written = (JsInstMap&)node;
                operands(node);
                text(" ["_v);

                for(U32 i = 0; i < written.entries.size(); i++) {
                    if(i) text(", "_v);
                    auto entry = written.entries.get(base, i);
                    real(entry.from);
                    text(" => "_v);
                    constant(entry.to);
                }

                text(", else "_v);
                constant(written.fallback);
                text("]"_v);
                return;
            }

            case JsInst::Split:
                operands(node);
                text(" #"_v);
                uint(((JsInstSplit&)node).part);
                text(" of "_v);
                rep(((JsInstSplit&)node).parts);
                return;

            case JsInst::Materialize:
                text(" "_v);
                rep(((JsInstMaterialize&)node).parts);
                operands(node);
                return;

            case JsInst::Math:
                text("."_v);
                text(jsMathOpName(((JsInstMath&)node).op));
                operands(node);
                return;

            case JsInst::Jump:
                edge(((JsInstJump&)node).to);
                return;

            case JsInst::Branch:
                operands(node);
                edge(((JsInstBranch&)node).whenTrue);
                edge(((JsInstBranch&)node).whenFalse);
                return;

            case JsInst::Switch: {
                auto& written = (JsInstSwitch&)node;
                operands(node);

                auto items = written.cases.pointer(base);
                for(U32 i = 0; i < written.cases.size(); i++) {
                    text(" case "_v);
                    real(items[i].value);
                    edge(items[i].edge);
                }

                text(" else"_v);
                edge(written.fallback);
                return;
            }

            default:
                operands(node);
                return;
        }
    }

    void block(JsBlock& value) {
        text("  block"_v);
        uint(value.index.index);

        if(value.argumentCount()) {
            text("("_v);
            for(U32 i = 0; i < value.argumentCount(); i++) {
                if(i) text(", "_v);
                auto argument = base[value.argumentAt(base, i)];
                valueRef(value.argumentAt(base, i));
                text(": "_v);
                rep(argument->rep);
            }

            text(")"_v);
        }

        text(":\n"_v);

        for(auto instPointer: value.instructions(base)) {
            inst(*base[instPointer]);
            text("\n"_v);
        }
    }

    void abi(JsAbiId id) {
        auto& plan = target.abi(id);

        text("("_v);
        auto first = true;
        for(auto& parameter: plan.parameters) {
            if(!first) text(", "_v);
            first = false;

            text(jsPassingKindName(parameter.kind));
            if(parameter.kind == JsPassingKind::Omitted) continue;

            text(" "_v);
            rep(parameter.rep);
        }

        text(") -> "_v);
        text(jsPassingKindName(plan.result.kind));

        if(plan.result.kind != JsPassingKind::Omitted) {
            text(" "_v);
            rep(plan.result.rep);
        }

        if(plan.takesEnv) text(" env"_v);
        text("\n"_v);
    }
};

} // namespace

void printRep(Net::Writer& writer, JsTarget& target, JsRepId id) {
    Printer printer(writer, target);
    printer.repDeclaration(id);
}

void printTables(Net::Writer& writer, JsTarget& target) {
    Printer printer(writer, target);

    /*
     * What the optimizer did, over the whole program - §9.9's "each optimization needs a counter".
     *
     * In the dump rather than in `YanaBench`, because every number this slice is judged on is about
     * the *program* and not about the compiler: `YanaBench` has no JSIR row and adding one would
     * measure the phase. One line, so that a measurement is `grep optimizer` over a `-mode jsir`
     * run of whatever program is being asked about.
     */
    printer.text("optimizer clones "_v);
    printer.uint(target.optimizer.clones);
    printer.text(" aliased "_v);
    printer.uint(target.optimizer.aliased);
    printer.text(" forwarded "_v);
    printer.uint(target.optimizer.forwarded);
    printer.text(" removed "_v);
    printer.uint(target.optimizer.removed);
    printer.text(" adopted "_v);
    printer.uint(target.optimizer.adopted);
    printer.text(" sunk "_v);
    printer.uint(target.optimizer.sunk);
    printer.text(" numbered "_v);
    printer.uint(target.optimizer.numbered);
    printer.text(" decided "_v);
    printer.uint(target.optimizer.decided);
    printer.text(" unboxed "_v);
    printer.uint(target.optimizer.unboxed);
    printer.text(" updated "_v);
    printer.uint(target.optimizer.updated);
    printer.text(" split "_v);
    printer.uint(target.optimizer.split);
    printer.text(" promoted "_v);
    printer.uint(target.optimizer.promoted);
    printer.text("\n\n"_v);

    for(U32 i = 0; i < target.shapes.size(); i++) printer.shapeDeclaration(JsShapeId(i));
    if(target.shapes.size()) printer.text("\n"_v);

    // Printed only where something packed, so a program the packing switches leave alone dumps
    // exactly what it dumped before they existed.
    for(U32 i = 0; i < target.packings.size(); i++) printer.packingDeclaration(i);
    if(target.packings.size()) printer.text("\n"_v);

    for(U32 i = 0; i < target.reps.size(); i++) {
        printer.repDeclaration(JsRepId(i));
        printer.text("\n"_v);
    }
}

void printBody(Net::Writer& writer, JsTarget& target, JsBody& body) {
    Printer printer(writer, target);
    printer.body = &body;

    printer.text("fn "_v);
    if(body.function) printer.name(target.decl[body.function]->name);
    printer.abi(body.abi);

    for(U32 i = 0; i < body.variables.size(); i++) {
        printer.text("  var $"_v);
        printer.uint(i);
        printer.text(": "_v);
        printer.rep(body.variables.get(*target.arena, i));
        printer.text("\n"_v);
    }

    for(U32 i = 0; i < body.blocks.size(); i++) {
        printer.block(*body.block(*target.arena, JsBlockId(i)));
    }

    printer.text("\n"_v);
}

} // namespace jsir
