#include "lower_print.h"
#include "lower_inst.h"

inline bool isInlineKind(U32 kind) {
    return kind == LowerInst::Imm || kind == LowerInst::Global || kind == LowerInst::Fun;
}

inline bool isInlineInst(LowerInst& v) {
    return isInlineKind(v.kind) && v.createdCount == 1 && v.created()[0].uses.size() == 1;
}

inline bool shouldInlinePrint(LowerInst& inst, PrintContext& print) {
    if(!print.annotateGen) return true;
    if((inst.kind == LowerInst::Imm || inst.kind == LowerInst::Fun) && (((LowerImm&)inst).result.flags & LowerValue::Implicit)) return true;
    return false;
}

static void printIndentation(Net::Writer& writer, PrintContext& print) {
    auto c = print.depth * 2;
    for(auto i = 0; i < c; i++) {
        writer.writeByte(' ');
    }
}

static void printInt(Net::Writer& writer, U64 i) {
    writer.writeBytes(64, [&](Byte* c) {
        return show(i, (char*)c, 64);
    });
}

static void printInlineInst(Net::Writer& writer, Context& context, LowerBase base, LowerInst& inst) {
    if(inst.kind == LowerInst::Global) {
        writer.writeByte('@');
        writer.writeString(context.findName(base[((LowerInstGlobal&)inst).target]->name));
    } else if(inst.kind == LowerInst::Fun) {
        writer.writeString(context.findName(base[((LowerInstFun&)inst).target]->name));
    } else if(inst.kind == LowerInst::Imm) {
        writer.writeBytes(64, [&](Byte* c) {
            auto imm = (const LowerImm&)inst;

            if(isFloat(imm.result.type)) {
                return show(imm.f, (char*)c, 64);
            } else {
                return show(imm.i, (char*)c, 64);
            }
        });
    } else {
        assertTrue("not an inline instruction" == nullptr);
    }
}

static void printValueRef(Net::Writer& writer, Context& context, LowerBase base, LowerValue& v, PrintContext& print) {
    auto inst = v.inst();

    if(isInlineInst(*inst) && shouldInlinePrint(*inst, print)) {
        printInlineInst(writer, context, base, *inst);
    } else {
        writer.writeByte('%');

        if(v.name) {
            writer.writeString(context.findName(v.name));
        } else {
            auto id = print.valueMap.add(&v);
            if(!id.existed) *id.value = print.nameCounter++;

            writer.writeString("v_"_v);
            printInt(writer, *id.value);
        }
    }
}

static void printBlockRef(Net::Writer& writer, Context& context, const LowerBlock& v, PrintContext& print) {
    if(v.name) {
        writer.writeString(context.findName(v.name));
    } else {
        auto id = print.blockMap.add(&v);
        if(!id.existed) *id.value = print.nameCounter++;

        writer.writeString("b_"_v);
        printInt(writer, *id.value);
    }
}

// One successor of a conditional branch: `block` on its own, or `[block, weight]` when the branch
// states how likely its edges are - see EdgeLikelihood.
static void printEdge(Net::Writer& writer, Context& context, const LowerBlock& block,
    const EdgeLikelihood& likelihood, bool stated, PrintContext& print)
{
    if(!stated) {
        printBlockRef(writer, context, block, print);
        return;
    }

    writer.writeByte('[');
    printBlockRef(writer, context, block, print);
    writer.writeString(", "_v);
    printInt(writer, likelihood.weight);
    writer.writeByte(']');
}

static StringView nameForCmp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:
            return "cmp_eq"_v;
        case LowerCmp::neq:
            return "cmp_neq"_v;
        case LowerCmp::uno:
            return "cmp_uno"_v;
        case LowerCmp::ord:
            return "cmp_ord"_v;
        case LowerCmp::gt:
            return "cmp_gt"_v;
        case LowerCmp::ge:
            return "cmp_ge"_v;
        case LowerCmp::lt:
            return "cmp_lt"_v;
        case LowerCmp::le:
            return "cmp_le"_v;
        case LowerCmp::igt:
            return "cmp_igt"_v;
        case LowerCmp::ige:
            return "cmp_ige"_v;
        case LowerCmp::ilt:
            return "cmp_ilt"_v;
        case LowerCmp::ile:
            return "cmp_ile"_v;
    }

    assertTrue(false);
    return ""_v;
}

// The reduction is part of the name rather than a printed field, the way a comparison's condition
// is: there are eight of them, they are what the instruction *is*, and one name reads better than
// `vreduce %v, 3` in every dump this backend produces.
static StringView nameForReduce(LowerReduce reduce) {
    switch(reduce) {
        case LowerReduce::Add:  return "vreduce_add"_v;
        case LowerReduce::Mul:  return "vreduce_mul"_v;
        case LowerReduce::Min:  return "vreduce_min"_v;
        case LowerReduce::IMin: return "vreduce_imin"_v;
        case LowerReduce::Max:  return "vreduce_max"_v;
        case LowerReduce::IMax: return "vreduce_imax"_v;
        case LowerReduce::And:  return "vreduce_and"_v;
        case LowerReduce::Or:   return "vreduce_or"_v;
        case LowerReduce::Bits: return "vreduce_bits"_v;
    }

    assertTrue(false);
    return ""_v;
}

static StringView nameForCast(LowerBase base, const LowerInstCast& inst) {
    auto to = inst.result.type;
    auto from = base[inst.from]->type;

    if(isInt(from) && isFloat(to) && inst.isSignedSource()) {
        return "itof"_v;
    } else if(isInt(to) && isFloat(from) && inst.isSignedResult()) {
        return "ftoi"_v;
    } else if(isInt(to) && isInt(from) && inst.isSignedSource() && inst.isSignedResult()) {
        return "sext"_v;
    } else {
        return "cast"_v;
    }
}

StringView nameForInst(LowerBase base, LowerInst& inst) {
    switch(inst.kind) {
        case LowerInst::Arg:
            return "arg"_v;
        case LowerInst::Global:
            return "global"_v;
        case LowerInst::Fun:
            return "fun"_v;
        case LowerInst::Imm:
            return "imm"_v;
        case LowerInst::Nop:
            return "nop"_v;
        case LowerInst::Cast:
            return nameForCast(base, (LowerInstCast&)inst);
        case LowerInst::Bitcast:
            return "bitcast"_v;
        case LowerInst::Set:
            return "set"_v;
        case LowerInst::Neg:
            return "neg"_v;
        case LowerInst::Not:
            return "not"_v;
        case LowerInst::Sqrt:
            return "sqrt"_v;
        case LowerInst::Fma:
            return "fma"_v;
        case LowerInst::Add:
            return "add"_v;
        case LowerInst::Sub:
            return "sub"_v;
        case LowerInst::Mul:
            return "mul"_v;
        case LowerInst::IMul:
            return "imul"_v;
        case LowerInst::Div:
            return "div"_v;
        case LowerInst::IDiv:
            return "idiv"_v;
        case LowerInst::Rem:
            return "rem"_v;
        case LowerInst::IRem:
            return "irem"_v;
        case LowerInst::MulHi:
            return "mulhi"_v;
        case LowerInst::IMulHi:
            return "imulhi"_v;
        case LowerInst::Shl:
            return "shl"_v;
        case LowerInst::Shr:
            return "shr"_v;
        case LowerInst::Sar:
            return "sar"_v;
        case LowerInst::And:
            return "and"_v;
        case LowerInst::Or:
            return "or"_v;
        case LowerInst::Xor:
            return "xor"_v;
        case LowerInst::Cmp:
            return nameForCmp(((LowerInstCmp&)inst).getCmp());
        case LowerInst::Select:
            return "select"_v;
        case LowerInst::VecSplat:
            return "vsplat"_v;
        case LowerInst::VecLane:
            return "vlane"_v;
        case LowerInst::VecWithLane:
            return "vwithlane"_v;
        case LowerInst::VecShuffle:
            return "vshuffle"_v;
        case LowerInst::VecReduce:
            return nameForReduce(((LowerInstVecReduce&)inst).getReduce());
        case LowerInst::Alloca:
            return "alloca"_v;
        case LowerInst::Load:
            if(((LowerInstLoad&)inst).isOverread()) return "loadx"_v;
            return ((LowerInstLoad&)inst).isSigned() ? "loads"_v : "load"_v;
        case LowerInst::Store:
            return "store"_v;
        case LowerInst::Copy:
            return "copy"_v;
        case LowerInst::SetPattern:
            return "setpattern"_v;
        case LowerInst::X86PushArg:
            return "x86_pusharg"_v;
        case LowerInst::Call:
            return nameForCall(((LowerInstCall&)inst).getCallType());
        case LowerInst::Je:
            return "je"_v;
        case LowerInst::Jmp:
            return "jmp"_v;
        case LowerInst::Ret:
            return "ret"_v;
        case LowerInst::Unreachable:
            return "unreachable"_v;
        case LowerInst::Phi:
            return "phi"_v;
        case LowerInst::X86Address:
            return "x86_addr"_v;
        case LowerInst::X86Lea:
            return "x86_lea"_v;
        case LowerInst::Intrinsic:
            return lowerIntrinsicDesc(((LowerInstIntrinsic&)inst).getIntrinsic()).name;
    }

    assertTrue(false);
    return ""_v;
}

StringView nameForType(LowerType type) {
    assertTrue(!isVectorLike(type)); // a vector has no single name - see writeType

    switch(type.lane) {
        case LowerLane::Int32:
            return "Int"_v;
        case LowerLane::Int64:
            return "Long"_v;
        case LowerLane::Float32:
            return "Float"_v;
        case LowerLane::Float64:
            return "Double"_v;
        case LowerLane::Pointer:
            return "Ptr"_v;
        case LowerLane::Int8:
        case LowerLane::Int16:
            break; // only ever a lane, so only ever reached through writeType
    }

    assertTrue(false);
    return ""_v;
}

StringView nameForLane(LowerLane lane) {
    switch(lane) {
        case LowerLane::Int8:    return "i8"_v;
        case LowerLane::Int16:   return "i16"_v;
        case LowerLane::Int32:   return "i32"_v;
        case LowerLane::Int64:   return "i64"_v;
        case LowerLane::Float32: return "f32"_v;
        case LowerLane::Float64: return "f64"_v;
        case LowerLane::Pointer: return "ptr"_v;
    }

    assertTrue(false);
    return ""_v;
}

/*
 * The whole type.
 *
 * A scalar is written the way it always was, which is what keeps every existing golden byte for
 * byte. A vector is `f32x8` and a mask `m32x8` - short, unambiguous, and the spelling the parser
 * reads back. A mask names the *width* of the lane it masks rather than its kind, because that and
 * the lane count are the whole of a mask's identity (Design-Vector §2.4): a comparison of two
 * `f32x8` and one of two `i32x8` produce the same type.
 */
void writeType(Net::Writer& writer, LowerType type) {
    if(!isVectorLike(type)) {
        writer.writeString(nameForType(type));
        return;
    }

    if(type.isMask()) {
        writer.writeByte('m');
        printInt(writer, laneBytes(type.lane) * 8);
    } else {
        writer.writeString(nameForLane(type.lane));
    }

    writer.writeByte('x');
    printInt(writer, type.lanes());
}

StringView nameForCallType(LowerCallType type) {
    switch(type) {
        case LowerCallType::Sysv:
            return "sysv"_v;
        case LowerCallType::Win64:
            return "win64"_v;
        case LowerCallType::Simple:
            return "simple"_v;
        case LowerCallType::Complex:
            return "complex"_v;
        case LowerCallType::Clobber:
            return "clobber"_v;
        case LowerCallType::Syscall:
            return "system"_v;
    }

    assertTrue(false);
    return ""_v;
}

StringView nameForCall(LowerCallType type) {
    switch(type) {
        case LowerCallType::Sysv:
            return "call_sysv"_v;
        case LowerCallType::Win64:
            return "call_win64"_v;
        case LowerCallType::Simple:
            return "call_simple"_v;
        case LowerCallType::Complex:
            return "call_complex"_v;
        case LowerCallType::Clobber:
            return "call_clobber"_v;
        case LowerCallType::Syscall:
            return "syscall"_v;
    }

    assertTrue(false);
    return ""_v;
}

void printModule(Net::Writer& writer, Context& context, LowerBase base, LowerModule& module, PrintAnnotations annotations) {
    PrintContext print;

    for(auto g: module.globalOrder) {
        printGlobal(writer, context, base, *base[g], print);
        writer.writeByte('\n');
    }

    writer.writeByte('\n');

    for(auto f: module.functionOrder) {
        Ptr<Liveness> l;
        FunctionFrequencyInfo frequency;

        if(annotations.liveness) {
            l = base[f]->buildLiveness(base);
            print.live = l.get();
        }

        if(annotations.frequency) {
            frequency = base[f]->buildFrequencies(base);
            print.frequency = &frequency;
        }

        printFunction(writer, context, base, *base[f], print);
        writer.writeString("\n\n"_v);
    }
}

void printFunction(Net::Writer& writer, Context& context, LowerBase base, LowerFunction& decl, PrintContext& print) {
    print.nameCounter = 0;
    print.valueMap.clear();
    print.blockMap.clear();

    // Prefix data is printed where it is emitted - in front of the entry point rather than with the
    // module's globals - because being there is the whole of what it is. Like the relocations of any
    // other compiler-built table, it is shown rather than round-tripped: the text format describes
    // hand-written IR, and neither has syntax for it.
    if(decl.prefix) {
        printGlobal(writer, context, base, *base[decl.prefix], print);
        writer.writeByte('\n');
    }

    printIndentation(writer, print);
    writer.writeString(context.findName(decl.name));
    writer.writeByte('<');
    writer.writeString(nameForCallType(decl.callType));
    writer.writeByte('>');
    writer.writeByte('(');
    auto argIndex = 0;

    for(auto a: decl.args.contents(base)) {
        if(argIndex++ > 0) writer.writeString(", "_v);

        printValueRef(writer, context, base, base[a]->result, print);
        writer.writeString(": "_v);
        writeType(writer, base[a]->result.type);
    }

    writer.writeByte(')');
    Size returnIndex = 0;

    for(auto r: decl.returnTypes.contents(base)) {
        if(returnIndex++ == 0) {
            writer.writeString(": "_v);
        } else {
            writer.writeString(", "_v);
        }

        writeType(writer, LowerType(r));
    }

    writer.writeString(" {\n"_v);
    print.depth++;

    Size blockIndex = 0;
    Size startIndex = 0;

    // If the implicit starting block is empty aside from the function arguments, don't print it.
    if(decl.blocks.size() > 0) {
        auto startBlock = base[decl.blocks.get(base, 0)];
        if(startBlock->instructions.isEmpty() && startBlock->terminator && base[startBlock->terminator]->kind == LowerInst::Jmp) {
            startIndex = 1;
        }
    }

    for(auto b: decl.blocks.contents(base)) {
        if(blockIndex > startIndex) writer.writeString("\n\n"_v);
        if(blockIndex++ >= startIndex) printBlock(writer, context, base, *base[b], print);
    }

    writer.writeByte('\n');
    print.depth--;
    printIndentation(writer, print);
    writer.writeByte('}');
}

void printGlobal(Net::Writer& writer, Context& context, LowerBase base, LowerGlobal& global, PrintContext& print) {
    printIndentation(writer, print);

    // The one thing about a global that is not in its bytes, and the one an emitter acts on:
    // `mut` clear is a promise that nothing writes the storage, which is what lets a load of it
    // be folded into its reader or rematerialized. It used to be printed nowhere and parsed
    // nowhere, so every global in a round-tripped module claimed to be immutable.
    if(global.mut) writer.writeString("mut "_v);

    writer.writeByte('@');
    writer.writeString(context.findName(global.name));
    writer.writeString(" = ["_v);

    for(Size i = 0; i < global.initialContents.length; i++) {
        printInt(writer, global.initialContents.ptr[i]);
        if(i < global.initialContents.length - 1) writer.writeString(", "_v);
    }

    writer.writeByte(']');
}

void printInst(Net::Writer& writer, Context& context, LowerBase base, LowerInst& inst, PrintContext& print) {
    printIndentation(writer, print);

    auto created = inst.created();
    Size currentCreated = 0;

    for(auto& c: created) {
        if(currentCreated++ > 0) writer.writeString(", "_v);
        printValueRef(writer, context, base, c, print);
    }

    if(currentCreated > 0) {
        writer.writeString(" = "_v);
    }

    writer.writeString(nameForInst(base, inst));
    writer.writeByte(' ');

    // Note: this checks the instruction *kind*, not isInlineInst() (which additionally requires
    // exactly one use). An Imm/Global/Fun instruction stores its value/target in a dedicated field
    // rather than in used(), so its own printed line must always go through printInlineInst() to
    // show that value - regardless of how many uses it has. isInlineInst() only decides whether
    // *other* references to this value get inlined at their use site instead of printed by name;
    // when it's false (0 or 2+ uses), this instruction still gets its own printed line here, and
    // that line must still show its target rather than leaving it blank.
    if(isInlineKind(inst.kind)) {
        printInlineInst(writer, context, base, inst);
    } else if(inst.kind == LowerInst::Phi) {
        auto& phi = (LowerInstPhi&)inst;
        auto values = phi.used();
        auto blocks = phi.sources();

        for(Size i = 0; i < values.length; i++) {
            if(i > 0) writer.writeString(", "_v);

            writer.writeByte('[');
            printBlockRef(writer, context, *base[blocks[i]], print);
            writer.writeString(", "_v);
            printValueRef(writer, context, base, *base[values[i]], print);
            writer.writeByte(']');
        }
    } else if(inst.kind == LowerInst::Jmp) {
        printBlockRef(writer, context, *base[((LowerInstJmp&)inst).then], print);
    } else if(inst.kind == LowerInst::Je) {
        auto& je = (LowerInstJe&)inst;

        // A branch that says nothing about its edges prints as a pair of bare labels, and one that
        // does prints both weights - the same all-or-nothing the parser accepts, so the text
        // round-trips exactly. A static estimate is deliberately not printed: it is derived from the
        // CFG on demand, and writing it down would turn a rederivable answer into a stored one that
        // the next CFG transform could leave stale.
        auto stated = je.hasLikelihood();

        printValueRef(writer, context, base, *base[je.cond], print);
        writer.writeString(", "_v);
        printEdge(writer, context, *base[je.then], je.likelihood[0], stated, print);
        writer.writeString(", "_v);
        printEdge(writer, context, *base[je.otherwise], je.likelihood[1], stated, print);

        // A branch reading the flags, and whether the comparison that set them went nowhere else.
        // "implicit" is the merged case, where the condition has no register at all; "flags" is the
        // one where it is materialized for some other reader and the branch simply does not use it.
        if(auto cmp = je.getEmbeddedCmp()) {
            auto folded = (base[je.cond]->flags & LowerValue::Implicit) != 0;
            writer.writeString(folded ? "    # implicit "_v : "    # flags "_v);
            writer.writeString(nameForCmp(cmp.unwrap()));
        }
    } else if(inst.kind == LowerInst::Select) {
        /*
         * `select <condition>, <lhs>, <rhs>`, which is not the order the operands are stored in.
         *
         * LowerInstSelect declares `lhs, rhs, cmp` because used values have to come first after the
         * embedded ones and the condition is the one that may be implicit; the *text* form puts the
         * condition first, because that is the order it reads in - `c ? a : b`. Printing `used()`
         * meant the printer and the parser disagreed, so a `.lower` file round-tripped into a
         * different program: `select %a, %v, %cmp` printed as `select %v, %cmp, %a`, which parses
         * back with the three operands rotated.
         */
        auto& select = (LowerInstSelect&)inst;

        printValueRef(writer, context, base, *base[select.cmp], print);
        writer.writeString(", "_v);
        printValueRef(writer, context, base, *base[select.lhs], print);
        writer.writeString(", "_v);
        printValueRef(writer, context, base, *base[select.rhs], print);
    } else {
        Size currentUse = 0;
        for(auto use: inst.used()) {
            if(currentUse++ > 0) writer.writeString(", "_v);
            printValueRef(writer, context, base, *base[use], print);
        }

        if(inst.kind == LowerInst::X86PushArg) {
            writer.writeString(", "_v);
            printInt(writer, ((LowerInstX86PushArg&)inst).stackOffset);
        } else if(inst.kind == LowerInst::Store) {
            writer.writeString(", "_v);
            printInt(writer, ((LowerInstStore&)inst).getWidth());
        } else if(inst.kind == LowerInst::Load) {
            writer.writeString(", "_v);
            printInt(writer, ((LowerInstLoad&)inst).getWidth());
        } else if(inst.kind == LowerInst::Alloca) {
            writer.writeString(", "_v);
            printInt(writer, ((LowerInstAlloca&)inst).alignment);
        } else if(inst.kind == LowerInst::VecLane || inst.kind == LowerInst::VecWithLane) {
            // The lane index is a field rather than an operand, so it comes after every operand the
            // loop above printed: `vlane %v, 3` and `vwithlane %v, %x, 3`. Same for the shuffle's
            // pattern below, which is why both read as a trailing run of numbers.
            writer.writeString(", "_v);
            printInt(writer, ((LowerInstVecLane&)inst).getLane());
        } else if(inst.kind == LowerInst::VecShuffle) {
            auto pattern = ((LowerInstVecShuffle&)inst).pattern();

            for(auto lane: pattern) {
                writer.writeString(", "_v);
                printInt(writer, lane);
            }
        }
    }

    auto needsComment = print.annotateGen;
    currentCreated = 0;

    for(auto& c: created) {
        writer.writeString(currentCreated++ ? ", "_v : ": "_v);
        writeType(writer, c.type);

        if(c.flags & LowerValue::Implicit) needsComment = true;
    }

    if(needsComment) {
        writer.writeString("    # "_v);

        currentCreated = 0;
        for(auto& c: created) {
            if(currentCreated++ > 0) writer.writeString(", "_v);

            if(c.flags & LowerValue::Implicit) {
                printValueRef(writer, context, base, c, print);
                writer.writeString(" implicit"_v);
            }
        }

        /*if(print.annotateGen) {
            writer.writeString("    # into "_v);

            for(Size i = 0; i < created.length; i++) {
                auto reg = created[i].reg;

                if(reg) {
                    if(auto name = print.registerNames.getValue(reg)) {
                        writer.writeString(name.unwrap());
                    } else {
                        printInt(writer, reg);
                    }
                } else {
                    writer.writeString("undefined"_v);
                }

                if(i < inst.createdCount - 1) writer.writeString(", "_v);
            }
        }*/
    }
}

static void printLiveness(Net::Writer& writer, Context& context, LowerBase base, LowerBlock& block, PrintContext& print, bool in) {
    printIndentation(writer, print);
    writer.writeString(in ? "# live-in: "_v : "# live-out: "_v);

    auto live = print.live->getBlock(&block);
    auto& set = in ? live->liveIn : live->liveOut;
    Size liveCount = 0;

    set.iterate(live->valueCount, [&](LiveId valueId) {
        if(liveCount > 0) writer.writeString(", "_v);
        liveCount++;
        printValueRef(writer, context, base, *print.live->getValue(valueId), print);
    });

    writer.writeByte('\n');
}

// A block's frequency as a multiple of the entry block's, to two decimal places - so the entry
// prints as 1.00, a body inside one loop as 8.00, and an arm the IR called unlikely as something
// well under one. Written by hand rather than through a float: these are exact ratios of integers,
// and a golden file has no business depending on how a float happens to be formatted.
static void printFrequency(Net::Writer& writer, U64 frequency) {
    printInt(writer, frequency / kEntryFrequency);
    writer.writeByte('.');

    auto fraction = (frequency % kEntryFrequency) * 100 / kEntryFrequency;
    if(fraction < 10) writer.writeByte('0');
    printInt(writer, fraction);
}

void printBlock(Net::Writer& writer, Context& context, LowerBase base, LowerBlock& block, PrintContext& print) {
    printIndentation(writer, print);
    printBlockRef(writer, context, block, print);

    writer.writeString(" {\n"_v);
    print.depth++;

    if(print.frequency) {
        printIndentation(writer, print);
        writer.writeString("# frequency: "_v);
        printFrequency(writer, print.frequency->frequencyOf(block.index));
        writer.writeByte('\n');
    }

    if(print.live) {
        printLiveness(writer, context, base, block, print, true);
    }

    for(auto i: block.phis.contents(base)) {
        printInst(writer, context, base, *base[i], print);
        writer.writeByte('\n');
    }

    for(auto offset: block.instructions.contents(base)) {
        auto i = base[offset];
        if(isInlineInst(*i) && shouldInlinePrint(*i, print)) continue;

        printInst(writer, context, base, *i, print);
        writer.writeByte('\n');
    }

    if(block.terminator) {
        printInst(writer, context, base, *base[block.terminator], print);
        writer.writeByte('\n');
    }

    if(print.live) {
        printLiveness(writer, context, base, block, print, false);
    }

    print.depth--;
    printIndentation(writer, print);
    writer.writeByte('}');
}
