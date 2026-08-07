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
struct LowerGlobal;

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
// One entry per block, and several of these exist per function - the postorder, the dominator
// table, the loop headers. Sixty-four inline rather than the thirty-two this started at, because
// thirty-two turned out to be under the ordinary function rather than over it: a `match` over a sum
// type is already a block per arm, and every function past the bound paid one allocation per list
// per round of every analysis. See SmallArray on why the bound is a guess.
using BlockList = SmallArray<BlockIndex, 64>;
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

/*
 * Edge likelihood.
 *
 * A conditional terminator may say how its two successors compare - which arm is the common one and
 * which is the exception - and where that claim came from. It is the one thing about a branch that
 * cannot be recovered from the CFG: `je %ok, cont, panic` and `je %c, left, right` are the same
 * instruction, and only whoever produced them knows that one of them almost never branches.
 *
 * Weights are relative to the terminator's other edge and nothing else. There is no unit and no
 * absolute count anywhere in this file: what an edge weight of 99 means is "99 times as often as its
 * sibling weighted 1", and the block frequencies derived below are stated relative to the function's
 * own entry for the same reason. A frequency that meant "times per second" would be a claim the
 * compiler is in no position to make.
 *
 * The source orders precedence when two answers exist. A measured profile beats a frontend hint,
 * which beats an estimate derived from the shape of the CFG, which beats nothing at all - so a later
 * profiling pass writes into the same field rather than into one of its own, and the allocator that
 * reads a weight never learns which of the four produced it.
 */
enum class LikelihoodSource: U8 {
    // Nothing is known, and the edge is as likely as its sibling.
    Unknown,

    // Derived from the shape of the CFG - a loop backedge, a loop exit, an edge into unreachable or
    // non-returning code. Never stored on an edge: it is rederivable by definition, so keeping it
    // would be a second copy of an answer the analysis produces anyway (see edgeWeightsOf).
    Static,

    // Stated by whoever produced the IR: `likely`/`unlikely`, an exception path, a panic path, a
    // failed bounds check - semantic knowledge a generic CFG walk cannot get back.
    FrontendHint,

    // Measured. Nothing produces this yet; it is named here so that the pass that eventually does
    // has somewhere to put its answer that every consumer already reads.
    Profile,
};

struct EdgeLikelihood {
    U32 weight = 1;
    LikelihoodSource source = LikelihoodSource::Unknown;
};

// The largest weight an edge may state. Frequencies are computed as `frequency * weight / total` in
// 64 bits, so bounding the weight is what keeps that product from overflowing at the frequency
// ceiling below; 2^20 is far more resolution than any hint or profile can honestly claim.
static constexpr U32 kMaxEdgeWeight = 1 << 20;

/*
 * Loops.
 *
 * One loop per block a back edge targets, its body the union of the natural loops of every back edge
 * to it - so a loop with two latches is one loop rather than two, and a block in it is one loop deep
 * rather than two. Loops found this way are properly nested or disjoint, which is what lets the
 * innermost one containing a block name the whole chain.
 *
 * A CFG with an irreducible loop has no such structure. Nothing here fails on one - the back edges a
 * depth-first walk happens to find are still grouped and still nest - but the answer is then one
 * possible reading of the loop rather than the only one, which is all a heuristic needs.
 */
struct LoopInfo {
    // The innermost loop containing each block, named by its header, or kNullBlock for a block in no
    // loop at all. A header is a member of its own loop, so `header[h] == h` for every header.
    BlockList header;

    // For a loop header, the header of the loop immediately containing it, or kNullBlock. Only
    // meaningful where `header[b] == b`.
    BlockList parent;

    // How many loops each block is inside: the length of the chain the two lists above describe.
    // Parallel to the two BlockLists above, so it carries the same bound.
    SmallArray<U16, 64> depth;

    bool isHeader(BlockIndex b) const { return header[b] == b; }

    // Whether `block` is inside the loop headed by `loop`. A header counts as inside its own loop,
    // which is what makes a latch's back edge an edge that stays in the loop rather than leaves it.
    bool contains(BlockIndex loop, BlockIndex block) const;

    // Whether the edge from `from` to `to` closes a loop - `to` heads a loop that `from` is inside.
    // This is the definition of a back edge, asked of the finished structure rather than of the walk
    // that found it, so anything reading the loops answers it the same way.
    bool isBackEdge(BlockIndex from, BlockIndex to) const {
        return isHeader(to) && contains(to, from);
    }
};

// The relative weights of a block's two outgoing edges, as edgeWeightsOf answers them. Index 0 is
// the block's `outgoing[0]`, index 1 its `outgoing[1]`; a block with fewer than two successors has
// nothing to weigh and answers with the neutral pair.
struct EdgeWeights {
    U32 weight[2] = { 1, 1 };
    LikelihoodSource source = LikelihoodSource::Unknown;

    U32 total() const { return weight[0] + weight[1]; }
};

// How often each block runs relative to the block the function is entered through. Absolute counts
// are unnecessary and unavailable; every consumer compares one block against another.
struct FunctionFrequencyInfo {
    // Indexed by LowerBlock::index. A block the entry cannot reach has frequency zero. Inline on
    // the same terms as BlockList, which it is parallel to - one of these is built per function
    // every time a pass asks what runs often.
    SmallArray<U64, 64> relativeBlockFrequency;

    U64 frequencyOf(BlockIndex block) const { return relativeBlockFrequency[block]; }

    FunctionFrequencyInfo() = default;
    FunctionFrequencyInfo(FunctionFrequencyInfo&&) = default;

    // Written out for the same reason Loop's is: the inherited assignment appends where the
    // allocator cannot swap buffers, so a SmallArray deletes it and the replacement has to say
    // which of the two it means. The printer assigns one of these into a variable that outlives
    // the `if` that built it, which is the only caller.
    FunctionFrequencyInfo& operator = (FunctionFrequencyInfo&& other) {
        if(this != &other) replaceContents(relativeBlockFrequency, other.relativeBlockFrequency);
        return *this;
    }
};

// The entry block's frequency, and so the unit every other one is stated in: a block that runs
// exactly as often as the function is entered has this frequency, one that runs half as often has
// half of it. Large enough that a dozen nested even branches still divide down to something above
// zero, which is what keeps a rarely-taken path distinguishable from an unreachable one.
static constexpr U64 kEntryFrequency = U64(1) << 16;

// What a loop is assumed to iterate when nothing better is known. It is the one number here that is
// a guess rather than a derivation, and it is deliberately a single one: an analysis that inferred
// trip counts per loop would be a different piece of work, and every consumer of the result compares
// blocks rather than trusting the count. Eight is also what the loop-depth weighting it replaces
// used, so a loop with no branches in it weighs exactly what it always did.
static constexpr U64 kLoopTripCount = 8;

// The ceiling frequencies saturate at, rather than wrapping. Deeply nested loops multiply by the
// trip count per level, so nine of them reach this - and a block that runs 2^40 times as often as
// the function's entry is already as hot as any consumer can express.
static constexpr U64 kMaxFrequency = U64(1) << 40;

// How `block`'s two outgoing edges compare, from whichever of the four sources knows most about
// them. This is the single statement of what a branch's probabilities are: block layout, the
// frequency computation and anything later that weighs one edge against another all ask it, so no
// two of them can disagree about which arm of a branch is the likely one.
EdgeWeights edgeWeightsOf(LowerBase base, const LoopInfo& loops, LowerBlock* block);

/*
 * Program points.
 *
 * Each instruction owns two: `before`, where its operands are read, and `after`, where its results
 * appear. Ranges are stated in these rather than in instruction indices, and half-open.
 *
 * The split is what lets a result take over the register of an operand that dies at the same
 * instruction - x86 reads every source before writing the destination, so the operand's range ends
 * exactly where the result's begins and the two never look like they overlap. With one point per
 * instruction the two would share it, and every two-address operation would need a copy.
 */
inline U32 beforeInst(U32 index) { return index * 2; }
inline U32 afterInst(U32 index) { return index * 2 + 1; }

// One stretch over which a value is live, half-open: [from, to).
struct Range {
    U32 from = 0;
    U32 to = 0;

    bool covers(U32 point) const { return point >= from && point < to; }
};

// A value's whole live range, as a sorted list of disjoint sub-ranges.
//
// The holes are the point. Modelling a value as one interval from its first definition to its last
// use makes it interfere with everything living in between, even where it is dead - and the worst
// case is the one that matters most: a loop-carried phi is live at the loop header and again at each
// predecessor's terminator, so one interval covers the entire loop body, and the value computed for
// the next iteration can never share its register. Every loop paid two moves an iteration for that.
//
// Held as a view into Liveness::rangeStore rather than as a container of its own, so that all of a
// function's ranges are one arena allocation and an interval is cheap to pass around.
struct LiveInterval {
    const Range* ranges = nullptr;
    U32 count = 0;

    bool isEmpty() const { return count == 0; }

    // The first index at which the value is live, and one past the last. Everything between them is
    // covered by some range or falls in a hole.
    U32 first() const { return ranges[0].from; }
    U32 last() const { return ranges[count - 1].to; }

    bool covers(U32 point) const {
        for(U32 i = 0; i < count; i++) {
            if(ranges[i].covers(point)) return true;
            if(ranges[i].from > point) break;
        }

        return false;
    }

    // True if the value has to hold its location *across* the instruction at `index`, rather than
    // merely being defined or consumed there. Only these have to dodge what the instruction writes:
    // one defined there, or read there for the last time, has nothing left to protect afterwards.
    bool crosses(U32 index) const {
        auto before = beforeInst(index);

        for(U32 i = 0; i < count; i++) {
            if(ranges[i].from <= before && ranges[i].to > before + 1) return true;
            if(ranges[i].from > before) break;
        }

        return false;
    }

    // Whether both values are live at any one index, and so cannot share a location. A merge walk
    // over two sorted lists, so it costs the length of the two rather than their product.
    bool overlaps(const LiveInterval& other) const {
        U32 a = 0, b = 0;

        while(a < count && b < other.count) {
            auto& left = ranges[a];
            auto& right = other.ranges[b];

            if(left.to <= right.from) a++;
            else if(right.to <= left.from) b++;
            else return true;
        }

        return false;
    }
};

struct LiveSet {
    LiveSet(LinearArena& a, Size valueCount): liveIn(a, valueCount), liveOut(a, valueCount), valueCount(valueCount) {}

    EmbedSet liveIn;
    EmbedSet liveOut;
    Size valueCount;

    // Range of the linear instruction numbering covered by this block: `firstIndex` is its first
    // instruction (or its terminator, for an empty block), `lastIndex` its terminator. Phi moves
    // for a successor's phis are emitted at `lastIndex`.
    U32 firstIndex = 0;
    U32 lastIndex = 0;
};

// Where one value's ranges sit in Liveness::rangeStore.
struct RangeSpan {
    U32 offset = 0;
    U32 count = 0;
};

struct Liveness {
    explicit Liveness(LinearArena& a): valueMap({ a }), blockMap({ a }), spans({ a }), rangeStore({ a }) {}

    void allocateBlocks(LinearArena& a, Size blockCount, Size valueCount) {
        assertTrue(blockMap.isEmpty());
        blockMap.reserve(blockCount);

        for(Size i = 0; i < blockCount; i++) {
            blockMap.push(LiveSet { a, valueCount });
        }
    }

    LinearArenaArray<LowerValue*> valueMap;
    LinearArenaArray<LiveSet> blockMap;

    // Where each value's ranges are, indexed by LiveId (parallel to valueMap), and the ranges of
    // every value in the function packed end to end. One allocation for all of them, since the
    // total is known before any of it is written.
    LinearArenaArray<RangeSpan> spans;
    LinearArenaArray<Range> rangeStore;

    // Number of instruction indices assigned; every index in [0, instCount) names one instruction
    // or terminator, in the order the blocks appear in LowerFunction::blocks.
    U32 instCount = 0;

    LiveSet* getBlock(LowerBlock* b);
    LowerValue* getValue(LiveId id);

    LiveInterval getInterval(LiveId id) {
        auto& span = spans[id];
        return LiveInterval { rangeStore.pointer() + span.offset, span.count };
    }
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
    StringId name {};

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
    StringId name {};

    // Source location where this block was defined.
    LocationId source = kNullLocation;

    // Temporary marker for use by tree traversals, to avoid external data structures.
    U32 marker = 0;

    // Index into the containing function's block list.
    // This always needs to be kept valid.
    BlockIndex index;

    // Index of the block within a postorder traversal of the containing function.
    // This is only valid between a call to LowerFunction::buildPostorder() and any transformations to the block tree.
    BlockIndex postIndex = kNullBlock;

    // How many loops this block is inside - so a block in two nested loops answers two. Written by
    // buildLoops, which whichever pass last ordered the blocks runs (the x64 backend's orderBlocks).
    // Kept on the block because the ordering itself needs it after the LoopInfo it came from has
    // been invalidated by the renumbering; anything weighing one part of a function against another
    // wants the block frequency rather than this, since a loop body under a cold branch is one loop
    // deep and still rarely executed.
    U16 loopDepth = 0;

    // Used while parsing blocks from source.
    RegionPtr<LowerParserRegion, LowerBlockAst> ast = nullptr;
};

struct LowerFunction {
    LowerFunction(Region<LowerRegion>& arena, LowerModule* module, StringId name):
        name(name), arena(arena), module(module)
    {
        // Functions always have an implicit entry point block, which contains the argument references.
        // The entry point block can never be jumped to.
        blocks.push(arena, new (arena) LowerBlock { this - *arena, StringId(), 0 } - *arena);
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

    // Finds the loops of the function - see LoopInfo. Additionally stores each block's loop depth in
    // LowerBlock::loopDepth. The result is indexed by block index, and so is invalidated by anything
    // that renumbers the blocks.
    LoopInfo buildLoops(LowerBase base);

    // How often each block runs relative to the entry block, combining the edge likelihoods the IR
    // carries with estimates derived from the loops for the edges that carry none.
    //
    // Derived rather than stored, the way liveness is: it is a function of the CFG and the edge
    // metadata, so a pass that changes either invalidates it, and one that changes neither cannot.
    FunctionFrequencyInfo buildFrequencies(LowerBase base);

    StringId name {};
    LocationId source = kNullLocation;

    Region<LowerRegion>& arena;
    LowerModule* module;

    LowerList<LowerPtr<LowerBlock>> blocks;
    LowerList<LowerPtr<LowerArg>> args;
    LowerList<LowerType> returnTypes;

    LowerCallType callType = kDefaultCallType;

    /*
     * Constant data emitted immediately in front of this function's entry point, or null.
     *
     * "Immediately" is the whole content of the field: a code generator may pad before the data but
     * never between the data and the first instruction, because what reads it computes its address
     * by subtracting the data's size from the entry point's. A closure header is the one user - see
     * ClosureHeaderLayout in resolve/witness.h - and prefix data is what LLVM calls the same thing.
     *
     * It is reached through the function rather than through LowerModule::globals for the same
     * reason: a global in that list is emitted wherever the module's data goes, and these bytes
     * have exactly one place they may be.
     */
    LowerPtr<LowerGlobal> prefix = nullptr;
};

/*
 * One address inside a global's initial contents that only the loader can fill in.
 *
 * A constant table holding the address of a function or of another global cannot state that
 * address as bytes: nothing knows where the module will be placed until it is. So the bytes are
 * left zero, the site is recorded here, and whoever maps the module writes `base + offset` into it.
 * That is the same thing an object file's `R_X86_64_64` says, spelled for a flat buffer.
 *
 * This is what makes the generic model's witness tables - a TypeDesc's `drop`, a class witness's
 * method slots - ordinary constant data rather than something assembled at run time on every call.
 */
struct LowerGlobal;

struct LowerDataRelocation {
    // Where in `initialContents` the address goes.
    U32 offset = 0;

    /*
     * A compiler-built table's slot rather than a pointer a source constant holds, and the two are
     * written completely differently.
     *
     * A table slot is four bytes holding `target - &anchor`, which is known as soon as both are
     * placed within the image - so it is written when the image is assembled and needs nothing at
     * load time. See repr/table.h for why anchor-relative rather than absolute.
     *
     * A source constant's pointer - the run inside a string literal, say - is the program's own
     * pointer type, so it is the target's full width and absolute, and it stays unknown until the
     * image is mapped. Both arrive in one list because both are "an address inside data"; only the
     * first can be resolved early, and treating them alike would either truncate a real pointer or
     * make every table wait for a load address it does not need.
     */
    bool anchorRelative = false;

    // Exactly one of these is set.
    LowerPtr<LowerFunction> function = nullptr;
    LowerPtr<LowerGlobal> global = nullptr;
};

struct LowerGlobal {
    explicit LowerGlobal(StringId name): name(name) {}

    StringId name {};

    /*
     * Whether anything writes this storage. Clear is a *promise* rather than a hint - it becomes
     * LLVM's `constant`, so a global that says so and is written anyway is a program whose reads all
     * fold to the initializer and whose stores are dropped as dead.
     *
     * Which makes it a different question from the source language's `let &`: see Global::isWritten,
     * where a global that no expression may assign to is still written once by the entry sequence.
     */
    bool mut = false;
    Location source;

    // Also defines the size; uninitialized globals are filled with zeroes.
    ByteBuffer initialContents;

    // The addresses inside `initialContents` that are not known until the module is placed.
    LowerList<LowerDataRelocation, false> relocations;
};

struct LowerModule {
    explicit LowerModule(Size maxMemory): arena(maxMemory) {}

    LowerFunction* addFunction(StringId funName);

    Region<LowerRegion> arena;

    // Finding one by name, which is how everything downstream of lowering refers to one.
    HashMap<StringId, LowerPtr<LowerGlobal>> globals;
    HashMap<StringId, LowerPtr<LowerFunction>> functions;

    /*
     * The order they are emitted in, which is the order they were declared in.
     *
     * Separate from the maps above because iterating a HashMap walks its buckets, so emission order
     * used to *be* hash order: the data section's layout, the order functions were assembled in and
     * every golden file in test/resolve were all pinned to the bucket a name landed in. Changing the
     * hash function reordered ~300 fixtures without changing a line of any of them.
     *
     * So the map answers "which global is called this" and the list answers "what comes next", and
     * no pass may use the first for the second. Both are written by the same two places -
     * addFunction below, and lowerProgram for globals.
     */
    Array<LowerPtr<LowerGlobal>> globalOrder;
    Array<LowerPtr<LowerFunction>> functionOrder;

    // The label every compiler-built table's address slots are measured from - see repr/table.h.
    // Null for a program with no tables, and on a target whose tables hold references rather than
    // offsets; a backend that finds relocations to write and no anchor has a real inconsistency.
    LowerPtr<LowerGlobal> imageAnchor = nullptr;

    StringId name {};

    /*
     * The name of the function a finished program starts at - `Program::entry`, under whatever name
     * lowering gave it (see uniqueFunctionName, which may have had to rename it).
     *
     * A name rather than a pointer because that is what everything downstream asks with: the code
     * generator emits functions into a module and finds one again by name, and a backend that has to
     * hold a lower-IR pointer in order to know which of them starts the program would be a second
     * way of naming the same thing. Zero for a library, which has no entry.
     */
    StringId entry {};

    U16 errorCount = 0;
    U16 warningCount = 0;
};
