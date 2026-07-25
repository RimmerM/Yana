#pragma once

#include "../compiler/context.h"
#include "../util/container.h"

struct LowerFunction;
struct LowerInst;
struct LowerInstPhi;
struct LowerValue;
struct LowerArg;
struct LowerBlockAst;
struct LowerInstAst;
struct LowerModule;
struct LowerBlock;

struct LowerParserRegion;
struct LowerRegion;

using LowerBase = RegionBase<LowerRegion>;

template<class T>
using LowerPtr = RegionPtr<LowerRegion, T>;

template<class T, bool allowEmbed = true>
using LowerList = SmallList<LowerRegion, T, allowEmbed>;

enum class LowerType: U8 {
    Int32,
    Int64,
    Float32,
    Float64,
    Pointer,
};

inline bool isInt(LowerType type) {
    return type == LowerType::Int32 || type == LowerType::Int64;
}

inline bool isFloat(LowerType type) {
    return type == LowerType::Float32 || type == LowerType::Float64;
}

inline bool isPtr(LowerType type) {
    return type == LowerType::Pointer;
}

inline bool isIntLike(LowerType type) {
    return isInt(type) || isPtr(type);
}

enum class LowerCallType {
    // System V calling convention used on Linux, macOS, etc.
    Sysv,

    // Calling convention used for 64-bit Windows.
    Win64,

    // Calling convention for "simple" functions, which retains most caller registers.
    Simple,

    // Calling convention for "complex" functions, which gives most registers to the callee.
    Complex,

    // Calling convention that clobbers all registers.
    Clobber,

    // Calling convention for system calls.
    Syscall,

    // Must always be last.
    LastType = Syscall,
};

static constexpr LowerCallType kDefaultCallType = LowerCallType::Complex;

using BlockIndex = I16;
using BlockList = Array<BlockIndex>;
using LiveId = U16;

static constexpr BlockIndex kNullBlock = maxLimit<BlockIndex>;
static constexpr LiveId kNullLive = 0xffff;

struct DominatorTree {
    // An ordered list of block indexes that represents the postorder traversal of the block graph.
    BlockList postorder;

    // The immediate dominator for each block in the graph, indexed by the postorder offset.
    BlockList tree;

    // The index of the function starting block in these lists.
    BlockIndex startIndex;
};

struct LiveSet {
    LiveSet(LinearArena& a, Size valueCount): liveIn(a, valueCount), liveOut(a, valueCount), valueCount(valueCount) {}

    EmbedSet liveIn;
    EmbedSet liveOut;
    Size valueCount;
};

struct Liveness {
    explicit Liveness(LinearArena& a): valueMap({ a }), blockMap({ a }) {}

    void allocateBlocks(LinearArena& a, Size blockCount, Size valueCount) {
        assertTrue(blockMap.isEmpty());
        blockMap.reserve(blockCount);

        for(Size i = 0; i < blockCount; i++) {
            blockMap.push(LiveSet { a, valueCount });
        }
    }

    LinearArenaArray<LowerValue*> valueMap;
    LinearArenaArray<LiveSet> blockMap;

    LiveSet* getBlock(LowerBlock* b);
    LowerValue* getValue(LiveId id);
};

// A local register containing the result of some operation.
struct LowerValue {
    enum Flag {
        // If set, the value is carried forward implicitly, and doesn't need an assigned register.
        Implicit = 1,
    };

    LowerValue(LowerInst* inst, LowerType type, StringId name): name(name), type(type) {
        // Currently, the size of a value is 24 bytes (on 64 bits).
        // The only instructions that can support large numbers of values are function calls;
        // a 16-bit offset allows us to embed >20k arguments into an instruction.
        Size offset = (Byte*)this - (Byte*)inst;
        assertTrue((offset & 7) == 0);

        offset /= 8;
        assertTrue(offset < maxLimit<U16>);

        inset = offset;
    }

    // Given the provided dominator tree, checks if this value dominates the provided instruction.
    // transientIndex must be set correctly in each instruction.
    bool dominates(LowerBase base, LowerInst* inst, const DominatorTree& dominators);

    // Returns the instruction that produces this value.
    LowerInst* inst() {
        return (LowerInst*)((Byte*)this - inset * 8);
    }

    // Returns the liveness id of this value, if any.
    LiveId liveId();

    // Each instruction that uses this value.
    LowerList<LowerPtr<LowerInst>> uses;

    // Source name for this value.
    StringId name;

    // Values are always embedded into the instruction that created them.
    // This contains the offset from this value back into the instruction, in uint_ptr intervals.
    U16 inset;

    // Set of Flag.
    U8 flags = 0;

    // Type of the value.
    LowerType type;
};

// A sequence of instructions that is executed without interruption.
struct LowerBlock {
    LowerBlock(LowerPtr<LowerFunction> fun, StringId name, BlockIndex index):
        fun(fun), index(index), name(name) {}

    LowerInst* addInst(LowerBase base, LowerInst* inst);

    // Given the provided dominator tree, checks if this block dominates the provided one.
    // transientIndex must be set correctly in each block.
    bool dominates(LowerBlock* block, const DominatorTree& dominators);

    LowerPtr<LowerFunction> fun;

    LowerPtr<LowerInst> terminator = nullptr;
    LowerList<LowerPtr<LowerInstPhi>> phis;
    LowerList<LowerPtr<LowerInst>, false> instructions;

    // All blocks that can branch to this one.
    LowerList<LowerPtr<LowerBlock>> incoming;

    // All blocks this one can possibly branch to.
    // Contains at most 2 blocks, as there is only one conditional branch instruction.
    LowerPtr<LowerBlock> outgoing[2] = { nullptr, nullptr };

    // Name of the block, if any.
    StringId name;

    // Source location where this block was defined.
    LocationId source = kNullLocation;

    // Temporary marker for use by tree traversals, to avoid external data structures.
    U32 marker = 0;

    // Starting index into the register allocation operations list.
    U32 ops = 0;

    // Index into the containing function's block list.
    // This always needs to be kept valid.
    BlockIndex index;

    // Index of the block within a postorder traversal of the containing function.
    // This is only valid between a call to LowerFunction::buildPostorder() and any transformations to the block tree.
    BlockIndex postIndex = kNullBlock;

    // Used while parsing blocks from source.
    RegionPtr<LowerParserRegion, LowerBlockAst> ast = nullptr;
};

struct LowerFunction {
    LowerFunction(Region<LowerRegion>& arena, LowerModule* module, StringId name):
        name(name), arena(arena), module(module)
    {
        // Functions always have an implicit entry point block, which contains the argument references.
        // The entry point block can never be jumped to.
        blocks.push(arena, new (arena) LowerBlock { this - *arena, 0, 0 } - *arena);
    }

    LowerArg* addArg(LowerBase base, StringId argName, LowerType type);
    LowerBlock* addBlock(LowerBase base, StringId blockName);

    // Performs a postorder traversal of the blocks in the function,
    // and stores the resulting index in LowerBlock::postIndex.
    // Returns the ordered list of block indexes.
    BlockList buildPostorder(LowerBase base);

    // Calculates the dominator tree for the current set of blocks in the function.
    // Returns the flattened tree with each block's immediate dominator.
    // Additionally, stores the closest dominator in each block.
    DominatorTree buildDominatorTree(LowerBase base);

    // Calculates liveness information for all values in the function.
    // The information is stored in each block.
    Ptr<Liveness> buildLiveness(LowerBase base);

    StringId name;
    LocationId source = kNullLocation;

    Region<LowerRegion>& arena;
    LowerModule* module;

    LowerList<LowerPtr<LowerBlock>> blocks;
    LowerList<LowerPtr<LowerArg>> args;
    LowerList<LowerType> returnTypes;

    LowerCallType callType = kDefaultCallType;
};

struct LowerGlobal {
    explicit LowerGlobal(StringId name): name(name) {}

    StringId name;
    bool mut = false;
    Location source;

    // Also defines the size; uninitialized globals are filled with zeroes.
    ByteBuffer initialContents;
};

struct LowerModule {
    explicit LowerModule(Size maxMemory): arena(maxMemory) {}

    LowerFunction* addFunction(StringId funName);

    Region<LowerRegion> arena;
    HashMap<StringId, LowerPtr<LowerGlobal>> globals;
    HashMap<StringId, LowerPtr<LowerFunction>> functions;

    StringId name = 0;
    U16 errorCount = 0;
    U16 warningCount = 0;
};
