#include "print.h"
#include "../resolve_mir/print.h"

namespace ssa {

namespace {

struct Printer {
    Net::Writer& writer;
    Context& context;
    mir::Program& program;
    DeclBase decl;
    SsaBase base;

    void uint(U64 value) {
        writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
    }

    void name(StringId id) {
        auto text = context.findName(id);
        writer.writeString(stringView(text));
    }

    void valueRef(Value& value) {
        writer.writeString("%"_v);
        uint(value.id.index);
    }

    void use(Use used) {
        writer.writeString(useKindName(used.kind()));
        writer.writeString(" "_v);

        if(!used.isSet()) {
            writer.writeString("<none>"_v);
            return;
        }

        valueRef(*base[used.value()]);
    }

    /*
     * A value's declaration: its occurrence, then its convention.
     *
     * The occurrence and not the type alone, so that a shared window and its owner are two lines a
     * reader can tell apart - which is the whole reason the value carries one. `printOccurrence`
     * prints nothing extra for the default, so a golden of a milestone with no loans in it reads
     * exactly as it did when this printed a bare type.
     */
    void declaration(Value& value) {
        valueRef(value);
        writer.writeString(": "_v);
        mir::printOccurrence(writer, context, decl, value.occurrence);
        writer.writeString(" "_v);
        writer.writeString(conventionName(value.convention));

        // A loan dependency, where there is one. `Guaranteed` without one is what the verifier will
        // report once loans exist; printing it is how a fixture shows which loan a borrow depends on.
        if(value.loan.valid()) {
            writer.writeString(" loan"_v);
            uint(value.loan.index);
        }
    }

    void inst(Inst& value) {
        writer.writeString("  "_v);

        auto results = value.results.size();
        for(Size i = 0; i < results; i++) {
            if(i) writer.writeString(", "_v);
            declaration(*base[value.results.get(base, i)]);
        }

        if(results) writer.writeString(" = "_v);

        // Two kinds print the operation rather than the row's base name - see inst.def.
        switch(value.kind) {
            case Inst::Binary:
                writer.writeString(mir::binaryOpName(((InstBinary&)value).op));
                break;
            case Inst::Unary:
                writer.writeString(mir::unaryOpName(((InstUnary&)value).op));
                break;
            default:
                writer.writeString(instMnemonic(value.kind));
                break;
        }

        // The fields that are not operands: what the instruction names statically. Written out per
        // kind rather than derived, because each is a different sort of name.
        switch(value.kind) {
            case Inst::Const:
                writer.writeString(" "_v);
                mir::printConstant(writer, decl, ((InstConst&)value).constant);
                break;
            case Inst::GlobalAddress:
                writer.writeString(" "_v);
                name(decl[((InstGlobalAddress&)value).global]->name);
                break;
            case Inst::Call:
                writer.writeString(" "_v);
                name(decl[((InstCall&)value).callee]->name);
                break;
            case Inst::Alloc: {
                auto& alloc = (InstAlloc&)value;
                writer.writeString(" "_v);
                writer.writeString(storageClassName(alloc.storage));
                writer.writeString(" "_v);
                mir::printOccurrence(writer, context, decl, alloc.held);
                break;
            }
            default:
                break;
        }

        forEachUse(base, value, [&](Use used) {
            writer.writeString(" "_v);
            use(used);
        });

        forEachEdge(value, [&](Edge& edge) {
            writer.writeString(" -> bb"_v);
            uint(base[edge.target]->index.index);

            if(edge.arguments.size()) {
                writer.writeString("("_v);
                auto first = true;
                for(auto used: edge.arguments.contents(base)) {
                    if(!first) writer.writeString(", "_v);
                    first = false;
                    use(used);
                }
                writer.writeString(")"_v);
            }
        });

        writer.writeString("\n"_v);
    }

    void block(Block& value) {
        writer.writeString("bb"_v);
        uint(value.index.index);

        if(value.arguments.size()) {
            writer.writeString("("_v);
            auto first = true;
            for(auto argument: value.arguments.contents(base)) {
                if(!first) writer.writeString(", "_v);
                first = false;
                declaration(*base[argument]);
            }
            writer.writeString(")"_v);
        }

        writer.writeString(":\n"_v);
        for(auto instPointer: value.instructions.contents(base)) inst(*base[instPointer]);
    }

    void function(mir::Function& value) {
        writer.writeString("fn "_v);
        name(value.name);
        writer.writeString("("_v);

        // The parameters as *declared*, not as the entry block's arguments. They are the same list -
        // the conversion makes one value per declaration - and this is the half that says what the
        // callee agreed to receive, which is what a call in another function is checked against.
        auto first = true;
        for(auto argumentPointer: value.args.contents(decl)) {
            auto argument = decl[argumentPointer];
            if(!first) writer.writeString(", "_v);
            first = false;

            name(argument->name);
            writer.writeString(": "_v);
            mir::printOccurrence(writer, context, decl, argument->occurrence);
        }

        writer.writeString(") -> "_v);
        mir::printOccurrence(writer, context, decl, value.result);

        if(!value.ssa) {
            writer.writeString(" = <no body>\n"_v);
            return;
        }

        writer.writeString(" {\n"_v);

        auto& body = *base[value.ssa];
        for(U32 i = 0; i < body.blocks.size(); i++) {
            if(i) writer.writeString("\n"_v);
            block(*body.block(base, BlockId(i)));
        }

        writer.writeString("}\n"_v);
    }

    void run() {
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];

            writer.writeString("module "_v);
            name(module->name);
            writer.writeString("\n"_v);

            for(auto globalPointer: module->globals.contents(decl)) {
                auto global = decl[globalPointer];
                writer.writeString("\n"_v);
                writer.writeString(global->writable ? "let & "_v : "let "_v);
                name(global->name);
                writer.writeString(": "_v);
                mir::printType(writer, context, decl, global->occurrence.type);

                if(global->initial.isSet()) {
                    writer.writeString(" = "_v);
                    mir::printConstant(writer, decl, global->initial);
                }

                writer.writeString("\n"_v);
            }

            for(auto functionPointer: module->functions.contents(decl)) {
                writer.writeString("\n"_v);
                function(*decl[functionPointer]);
            }
        }

        if(program.entry) {
            writer.writeString("\nentry "_v);
            name(decl[program.entry]->name);
            writer.writeString("\n"_v);
        }
    }
};

} // namespace

void printProgram(Net::Writer& writer, Context& context, mir::Program& program, Converted& converted) {
    Printer printer { writer, context, program, *program.decl, *converted.arena };
    printer.run();
}

} // namespace ssa
