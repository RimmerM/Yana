#pragma once

#include "lower.h"
#include "Net/Stream.h"

struct PrintContext {
    // Current nested block depth.
    U32 depth = 0;

    // Used to generate names for anonymous variables.
    U32 nameCounter = 0;

    // If set, prints comments with code generation info for each instruction.
    bool annotateGen = false;

    // If set, prints comments with liveness info for each block.
    Liveness* live = nullptr;

    // If set, prints each block's execution frequency relative to the function's entry.
    const FunctionFrequencyInfo* frequency = nullptr;

    // Maps anonymous variables to their temporary name.
    HashMap<const LowerValue*, U32> valueMap;

    // Maps anonymous blocks to their temporary name.
    HashMap<const LowerBlock*, U32> blockMap;

    // Maps register ids to their printable name (only used when annotateGen == true).
    HashMap<U8, StringView> registerNames;
};

// What a printout annotates each function with beyond its instructions. Each one costs an analysis
// the plain printout does not run, and each has a golden of its own in the test suite - so a change
// to one analysis shows up as a change to the file that exists to cover it rather than everywhere.
struct PrintAnnotations {
    bool liveness = false;
    bool frequency = false;

    bool any() const { return liveness || frequency; }
};

void printModule(Net::Writer& writer, Context& context, LowerBase base, LowerModule& module, PrintAnnotations annotations = {});
void printFunction(Net::Writer& writer, Context& context, LowerBase base, LowerFunction& decl, PrintContext& print);
void printGlobal(Net::Writer& writer, Context& context, LowerBase base, LowerGlobal& global, PrintContext& print);
void printInst(Net::Writer& writer, Context& context, LowerBase base, LowerInst& inst, PrintContext& print);
void printBlock(Net::Writer& writer, Context& context, LowerBase base, LowerBlock& block, PrintContext& print);

StringView nameForInst(LowerBase base, LowerInst& inst);
// A scalar type's name. A vector has none - see writeType, which is what every printer calls.
StringView nameForType(LowerType type);
StringView nameForLane(LowerLane lane);
void writeType(Net::Writer& writer, LowerType type);
StringView nameForCall(LowerCallType type);
