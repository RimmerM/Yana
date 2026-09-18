#include "internal.h"

namespace ssa {

/*
 * Which records a declared function takes and gives back as their fields - the aggregate half of
 * the calling convention.
 *
 * `README.md`'s first section is the convention this refines: a record has no register form, so an
 * argument of one is an address and a result of one comes back through a hidden pointer. Both are
 * right for a record that does not fit, and both are measurably wrong for one that does -
 * `test/bench/callconv/findings.md` has a register return 1.5-2.6x faster than the buffer at every
 * width it tried, and `test/bench/lexer/findings.md` §15 has a 58-byte `Token` zeroed, filled and
 * copied through a temporary on every call that reads it. `complex` already clobbers nine general
 * and twelve vector registers whether or not a call uses them, so a field sent in one of them costs
 * the caller nothing it was not already paying.
 *
 * ## A field at a time, not a word at a time
 *
 * A part is one **cell of the layout** - a scalar field, a co-packed word, a tag, a box pointer -
 * at the offset and width the layout gave it. Eight-byte words would be fewer registers for a record
 * of narrow fields, and they are what the benchmark's opaque rows modelled, but what the callee does
 * with the parts decides it: the entry stores each part into a slot of its own, and
 * `splitAggregateSlots` and `promoteStackSlots` only take that slot apart where its stores and loads
 * agree about cells. A field stored at its own width is read back at its own width and the slot
 * disappears into the registers it arrived in; a word stored whole and read a field at a time is a
 * slot that stays.
 *
 * ## Which ones, and why it is a function of the signature
 *
 * A caller and a callee decide this separately, so it has to be computed from what both can read -
 * the declaration and the layout - and nothing about either body. It is computed once per function,
 * before any body is lowered, and every call site and the callee's own entry read the same answer.
 *
 * The register budget is the convention's (`ConventionBudget`). Every parameter that is not a
 * candidate takes its register first, a candidate is counted as the pointer it would otherwise be,
 * and then candidates are upgraded **smallest first** while the whole list still fits - so splitting
 * one never pushes another argument out of a register. A function of one record may therefore spend
 * all nine general registers on it, and a function of eight scalars and a record keeps the record's
 * address. A result is split wherever it fits the result registers alone, which also takes the
 * hidden pointer out of the argument list.
 *
 * ## What is declined
 *
 * A function is declined where something reaches it by **address** rather than through one of the
 * call sites that read this: a lifted lambda is the code word of a function value, an erased body
 * and the allocator roles are called with shapes other files fix, a declared `@convention` is an ABI
 * somebody else wrote, and the entry is called by the runtime. A plain function named as a value is
 * *not* declined - its thunk is one of the readers.
 *
 * A parameter is a candidate where it is a copy the callee owns (`->x`), or a plain shared loan
 * bounded by the call (`x: T`) of a trivially copyable type. The second is the case that matters: it
 * is what the lexer's `advance(&self, token: Token)` is, and it arrives as a pointer to a temporary
 * the caller zeroed and filled. A loan that publishes an origin is declined because something may
 * outlive the call rooted in its storage, and `&x` because the callee writes back through it. A type
 * that may not be relocated (`isRelocatable`) is declined on either side, since the callee's copy is
 * at a different address.
 */

namespace {

// A layout that comes apart into more cells than this is left whole whatever the budget says - the
// bound `splitAggregateSlots` puts on the callee's slot, past which the cells would not promote.
constexpr Size kMaxCells = 16;

// How far into nested records and fixed arrays the walk goes before declining.
constexpr U32 kMaxDepth = 6;

bool isPowerOfTwo(U32 value) { return value && (value & (value - 1)) == 0; }

struct CellWalk {
    Layout layout;
    SmallArray<CarriedPart, 16>& cells;
    bool ok = true;

    /*
     * A pointer cell travels as the integer of its width. A part is only ever moved, and a pointer
     * is the one register type the lower IR has no literal for - so a returned `Nothing` whose
     * niche folds to zero would otherwise be an immediate that `validateLowerModule` refuses.
     */
    void cell(U32 offset, U32 bytes, LowerType type) {
        if(!bytes) return;
        if(cells.size() >= kMaxCells * 2 || !isPowerOfTwo(bytes)) { ok = false; return; }
        if(type.lane == LowerLane::Pointer && !type.laneShift) type = LowerType::Int64;
        cells.push(CarriedPart { offset, bytes, type });
    }

    void walk(const mir::Occurrence& occurrence, U32 at, U32 depth) {
        if(!ok) return;
        if(depth > kMaxDepth) { ok = false; return; }

        auto decl = layout.base;
        auto manifest = manifestNative(layout, occurrence);
        if(manifest.dynamic) { ok = false; return; }

        // A register value is one cell - a scalar, a vector, an enum-layout record, a loan.
        if(!manifest.inMemory) {
            cell(at, manifest.size, manifest.type);
            return;
        }

        auto& logical = *decl[occurrence.type];

        switch(logical.kind) {
            case mir::Type::Fun:
                if(((mir::FunType*)decl[occurrence.type])->convention) {
                    cell(at, layout.target.pointerSize, LowerType::Pointer);
                    return;
                }
                [[fallthrough]];
            case mir::Type::Iter:
                cell(at + layout.target.funCodeOffset(), layout.target.pointerSize, LowerType::Pointer);
                cell(at + layout.target.funEnvOffset(), layout.target.pointerSize, LowerType::Pointer);
                return;

            case mir::Type::Array: {
                auto& array = (mir::ArrayType&)logical;
                if(!array.hasFixedLength() || array.length > kMaxCells) { ok = false; return; }

                auto stride = elementStrideNative(layout, array.element);
                for(U64 i = 0; i < array.length && ok; i++) {
                    walk(mir::owned(array.element), at + U32(i * stride), depth + 1);
                }

                return;
            }

            case mir::Type::Record: {
                auto& record = (mir::RecordType&)logical;
                auto repr = reprOf(layout, occurrence.type, occurrence.shape);
                if(repr.opaque) { ok = false; return; }

                // A stored tag - a word of its own, or bits above a payload that is itself packed,
                // in which case the packed cell below names the same word and the two merge.
                if(repr.discriminant == Discriminant::Word || repr.discriminant == Discriminant::Bits) {
                    cell(at + repr.discriminantOffset, repr.discriminantBytes,
                         packLane(repr.discriminantBytes));
                }

                for(U16 i = 0; i < repr.fields.size() && ok; i++) {
                    auto field = repr.fields.get(decl, i);

                    if(field.isPacked()) {
                        if(field.isBytewise()) {
                            // A packed field that does not fit one legal access word is lowered as
                            // byte-wise read-modify-writes. Name those bytes individually so call
                            // carrying and alias analysis still sees the complete storage.
                            for(U32 byte = 0; byte < field.spanBytes(); byte++) {
                                cell(at + field.offset + byte, 1, LowerType::Int64);
                            }
                        } else {
                            cell(at + field.offset, field.wordBytes, packLane(field.wordBytes));
                        }
                    } else if(mir::indirectField(decl, record.field(decl, i))) {
                        cell(at + field.offset, layout.target.pointerSize, LowerType::Pointer);
                    } else {
                        walk(field.occurrence, at + field.offset, depth + 1);
                    }
                }

                if(repr.boxFlagsBytes) {
                    cell(at + repr.boxFlagsOffset, repr.boxFlagsBytes, packLane(repr.boxFlagsBytes));
                }

                return;
            }

            // An atomic is a location, and everything else in memory is either dynamic or has no
            // layout to take apart.
            default:
                ok = false;
                return;
        }
    }
};

/*
 * The cells, sorted and made disjoint - a sum's arms name overlapping bytes, and a packed tag names
 * the word its payload shares.
 *
 * Two cells at one offset and width are one cell; where their types differ it is moved as the
 * integer of that width, which is what `splitAggregateSlots` does with a conflicted cell too.
 *
 * Two that overlap any other way decline the record, and that is a measurement rather than caution.
 * The span of both is a width a load can name, but whichever arm was built wrote only its own field:
 * `LexCommand`'s one-byte payload read back as the four-byte cell it shares with its sibling is a
 * load wider than the store in front of it, which store forwarding cannot serve - on the lexer's
 * resume path that was 8.6% of the whole run, at an unchanged instruction count.
 */
bool normalizeCells(SmallArray<CarriedPart, 16>& cells) {
    for(Size i = 1; i < cells.size(); i++) {
        for(Size j = i; j > 0 && cells[j - 1].offset > cells[j].offset; j--) {
            auto held = cells[j]; cells[j] = cells[j - 1]; cells[j - 1] = held;
        }
    }

    SmallArray<CarriedPart, 16> merged;

    for(auto& next: cells) {
        if(merged.size()) {
            auto& last = merged[merged.size() - 1];

            if(next.offset < last.offset + last.bytes) {
                if(next.offset != last.offset || next.bytes != last.bytes) return false;
                if(isVectorLike(last.type) || isVectorLike(next.type)) return false;

                if(next.type != last.type) last.type = packLane(last.bytes);
                continue;
            }
        }

        merged.push(next);
    }

    if(merged.size() > kMaxCells) return false;
    replaceContents(cells, merged);
    return true;
}

// How many general and how many vector registers a list of cells takes.
RegisterBudget registersFor(const SmallArray<CarriedPart, 16>& cells) {
    RegisterBudget used;
    for(auto& part: cells) (isIntLike(part.type) ? used.gpr : used.vector)++;
    return used;
}

} // namespace

/*
 * The cells one occurrence travels as, or false where it does not come apart.
 *
 * Each cell keeps its own register type, so a record holding both kinds sends its floats in vector
 * registers beside its integers in general ones. The alternative - the whole record in general
 * registers, leaving the vector bank to records of floats and vectors alone - was measured on
 * `test/bench/callconv/Mixed.yana` and lost on both counts: 573 ms against 289, and 887 bytes of code
 * against 651, because every float field then crosses between the banks twice.
 */
bool Lowering::carriedCells(const mir::Occurrence& occurrence, SmallArray<CarriedPart, 16>& cells) {
    cells.clear();

    auto manifest = manifestNative(layout, occurrence);
    if(!manifest.inMemory || manifest.dynamic || !manifest.size) return false;
    if(!mir::isRelocatable(decl, occurrence.type)) return false;

    CellWalk walker { layout, cells };
    walker.walk(occurrence, 0, 0);
    if(!walker.ok || !cells.size() || !normalizeCells(cells)) return false;

    return true;
}

// The argument is in the file comment.
void Lowering::planCallShape(DeclPtr<mir::Function> pointer) {
    auto& function = *decl[pointer];
    if(pointer == program.entry || function.environment || function.convention) return;
    if(mir::isGeneric(decl, function.gen) && function.form == mir::GenForm::Erased) return;
    if(function.intrinsic != intrinsic::Op::Count) return;

    intrinsic::Known roles[] = { intrinsic::Known::allocateHeap, intrinsic::Known::freeHeap,
                                 intrinsic::Known::isHeapAddress, intrinsic::Known::checkFailed,
                                 intrinsic::Known::checkCondition, intrinsic::Known::checkFailedAt,
                                 intrinsic::Known::checkConditionAt };

    for(auto role: roles) {
        if(program.roles.function(role) == pointer) return;
    }

    /*
     * What the backend's convention hands out - `complex`'s tables, or what LLVM's `fastcc` places
     * in registers on x86-64 before it falls back to the stack or to a demoted return.
     *
     * Split under LLVM as well, and measured rather than assumed, since `lexer/findings.md` §16 has a
     * rewrite of exactly this neighbourhood that helped one backend and cost the other 23%: here the
     * LLVM columns do not move in time (lexer x1.01, programs x0.99, both inside their noise) and
     * `test/resolve` is 2.7% smaller, because a record handed over in registers is one LLVM no longer
     * has to prove it may take apart.
     */
    auto budget = context.settings.backend == NativeBackend::Llvm
        ? ConventionBudget { RegisterBudget { 6, 8 }, RegisterBudget { 4, 4 } }
        : complexBudget();

    SmallArray<CarriedPart, 16> cells;
    CallShape shape;
    shape.firstArg = U32(shapeArgs.size());
    shape.argCount = U32(function.args.size());

    U32 general = 0;
    U32 vector = 0;

    auto result = manifestNative(layout, function.result);
    auto resultSplit = false;

    if(result.inMemory && carriedCells(function.result, cells)) {
        auto used = registersFor(cells);

        if(used.gpr <= budget.results.gpr && used.vector <= budget.results.vector) {
            shape.resultStart = U32(shapeParts.size());
            shape.resultCount = U32(cells.size());
            for(auto& part: cells) shapeParts.push(part);
            resultSplit = true;
        }
    }

    // The hidden pointer the result would otherwise need.
    if(result.inMemory && !resultSplit) general++;

    struct Candidate {
        U32 position;
        U32 general;
        U32 vector;
        U32 first;
        U32 count;
    };

    SmallArray<Candidate, 8> candidates;
    SmallArray<CarriedPart, 32> pending;

    for(U32 i = 0; i < function.args.size(); i++) {
        auto& argument = *decl[function.args.get(decl, i)];
        auto& occurrence = argument.occurrence;
        auto manifest = manifestNative(layout, occurrence);

        shapeArgs.push(ShapeArg {});

        auto contract = occurrence.contract;
        auto owned = contract.isOwned() && manifest.inMemory;
        auto shared = !contract.isOwned() &&
                      contract.access() == mir::Access::Shared &&
                      contract.origin() == mir::Origin::CallBounded && !occurrence.shape &&
                      !isNarrowReference(layout, occurrence) &&
                      mir::isTriviallyCopyable(decl, occurrence.type) &&
                      isReferenceValue(layout, occurrence);

        if(!argument.lazy && (owned || shared) &&
           carriedCells(owned ? occurrence : mir::owned(occurrence.type), cells)) {
            auto used = registersFor(cells);
            candidates.push(Candidate { i, used.gpr, used.vector, U32(pending.size()), U32(cells.size()) });
            for(auto& part: cells) pending.push(part);
        }

        (isIntLike(manifest.type) ? general : vector)++;
    }

    // Smallest first, then in declaration order - which is what makes the choice deterministic.
    for(Size i = 1; i < candidates.size(); i++) {
        for(Size j = i; j > 0; j--) {
            auto& a = candidates[j - 1];
            auto& b = candidates[j];
            auto larger = a.count > b.count || (a.count == b.count && a.position > b.position);
            if(!larger) break;

            auto held = candidates[j]; candidates[j] = candidates[j - 1]; candidates[j - 1] = held;
        }
    }

    auto any = resultSplit;

    for(auto& candidate: candidates) {
        auto g = general - 1 + candidate.general;
        auto v = vector + candidate.vector;

        if(g > budget.args.gpr || v > budget.args.vector) continue;

        general = g;
        vector = v;
        any = true;

        auto& arg = shapeArgs[shape.firstArg + candidate.position];
        arg.start = U32(shapeParts.size());
        arg.count = candidate.count;
        for(U32 k = 0; k < candidate.count; k++) shapeParts.push(pending[candidate.first + k]);
    }

    if(!any) {
        shapeArgs.resize(shape.firstArg);
        return;
    }

    shapeIndex.add(pointer.offset, U32(shapes.size()));
    shapes.push(shape);
}

const CallShape* Lowering::callShapeOf(DeclPtr<mir::Function> function) {
    if(!function) return nullptr;
    auto found = shapeIndex.getValue(function.offset);
    return found ? &shapes[found.unwrap()] : nullptr;
}

// Each part of the record at `address`, loaded at its own width, in the order the callee's
// parameters were declared.
void Lowering::passCarried(LowerBlock& block, const CallShape* shape, U32 position,
                           LowerPtr<LowerValue> address, SmallArray<LowerPtr<LowerValue>, 8>& passed) {
    auto arg = shape && position < shape->argCount ? shapeArgs[shape->firstArg + position] : ShapeArg {};

    if(!arg.count) {
        passed.push(address);
        return;
    }

    for(U32 i = 0; i < arg.count; i++) passed.push(loadCarried(block, shapeParts[arg.start + i], address));
}

LowerPtr<LowerValue> Lowering::loadCarried(LowerBlock& block, const CarriedPart& part,
                                           LowerPtr<LowerValue> address) {
    auto loaded = block.addInst(lower, new (to.arena) LowerInstLoad(
        offsetAddress(block, address, part.offset), StringId(), part.type, part.bytes, false));
    return ((LowerInstSingle*)loaded)->created().ptr - lower;
}

void Lowering::storeCarried(LowerBlock& block, const CarriedPart& part, LowerPtr<LowerValue> value,
                            LowerPtr<LowerValue> storage) {
    block.addInst(lower, new (to.arena) LowerInstStore(offsetAddress(block, storage, part.offset), value,
                                                       part.bytes));
}

// A call's results, which are the parts of a record it returned, written back into storage.
void Lowering::storeReturnedParts(LowerBlock& block, const CallShape& shape, LowerInst* call,
                                  LowerPtr<LowerValue> storage) {
    for(U32 i = 0; i < shape.resultCount; i++) {
        storeCarried(block, resultPart(shape, i), call->created().ptr + i - lower, storage);
    }
}

/*
 * Whether a duplicate exists only to be taken apart - `let copy = x` handed straight to a parameter
 * the callee receives as its parts.
 *
 * The conversion duplicates a trivially copyable value at a shared position (`readMode`'s
 * `CopyTrivial`), and before this file that duplicate was the argument: a temporary zeroed, filled
 * and passed by address. Once the parameter arrives as parts the temporary's only reader is the row
 * of loads that takes those parts out of it, and they may as well read the original - provided the
 * original cannot change between the duplicate and the call. So the reader must be the call, in the
 * same block, naming the value at exactly one position that the callee splits, and everything
 * between the two must be an instruction that writes nothing: a load, a projection, or another
 * duplicate (whose own storage is fresh or, through `destinationFor`, the target of a `store` - and a
 * `store` ends the window).
 */
bool Lowering::carriedDuplicate(Inst& copy, Value& result) {
    if(result.useCount() != 1) return false;

    auto consumer = ssa[result.useAt(ssa, 0)];
    if(consumer->kind != Inst::Call || consumer->block != copy.block) return false;

    auto& call = (InstCall&)*consumer;
    if(call.through != mir::CallThrough::Declaration || locatedCheck(call.callee)) return false;

    auto shape = callShapeOf(call.callee);
    if(!shape) return false;

    auto self = SsaPtr<Value>(&result - ssa);
    U32 position = 0;
    U32 found = 0;
    auto split = false;

    for(auto used: call.arguments.contents(ssa)) {
        if(used.value() == self) {
            found++;
            split = position < shape->argCount && shapeArgs[shape->firstArg + position].count;
        }
        position++;
    }

    if(found != 1 || !split) return false;

    auto& block = *ssa[copy.block];
    auto from = SsaPtr<Inst>(&copy - ssa);
    auto to = SsaPtr<Inst>(consumer - ssa);
    auto inside = false;

    for(Size i = 0; i < block.instructionCount(); i++) {
        auto at = block.instructionAt(ssa, i);

        if(at == from) { inside = true; continue; }
        if(at == to) return inside;
        if(!inside) continue;

        switch(ssa[at]->kind) {
            case Inst::Load:
            case Inst::FieldAddress:
            case Inst::ElementAddress:
            case Inst::Copy:
                break;
            default:
                return false;
        }
    }

    return false;
}

} // namespace ssa
