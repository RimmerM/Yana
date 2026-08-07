/*
 * The driver: what is walked, in what order, and the two things that happen per function before its
 * body is translated at all - scalarization, and the erased environment being named.
 *
 * Everything a block's contents turn into is in the files lower_internal.h lists.
 */

#include "lower_internal.h"
#include "../compiler/stage.h"
#include "../opt/opt.h"
#include "../lower/lower_promote.h"
#include "../lower/lower_strength.h"
#include "../lower/lower_induction.h"

/*
 * Aggregates that evaporate.
 *
 * An owner whose representation has to be writable, addressable or resizable needs storage; one
 * that needs none of those is a value, and the fields it was built from can stay in registers. That
 * is the whole of what a Repr *variant* is at this milestone - there is no packing and no niche
 * yet, so the only two representations that differ are "in memory" and "not in memory at all".
 *
 * Which is why the demand analysis of Implementation-IR.md part 5 is what decides it rather than
 * anything lowering could work out for itself: read-only is a fact about every use of an owner
 * across the whole function, and `needsStableAddress` is the one that would otherwise be found out
 * too late, after the address had already been handed to someone.
 *
 * The rest of the conditions are lowering's own, and they are about what this translation can
 * express rather than about what is true:
 *
 *  - one basic block, because a field that flowed through a branch would need a phi per field, and
 *    building those is a real pass rather than a special case of this one;
 *  - a field path of exactly one Field projection, since a nested aggregate would have to be
 *    exploded recursively;
 *  - nothing that names the aggregate as a whole - passing it to a call, returning it, dropping it -
 *    because there is no whole left to name.
 */
static bool scalarizable(LowerContext& lower, Function& function, U32 index, OwnershipResult& ownership) {
    if(index >= ownership.locals.size()) return false;

    auto& tracked = ownership.locals[index];
    auto slot = function.localAt(lower.local, index);

    if(tracked.requirements.mutation != MutationDemand::ReadOnly) return false;
    if(tracked.requirements.needsStableAddress || tracked.requirements.mayResize) return false;
    if(tracked.escapes || tracked.droppable || slot.storage != StorageClass::Stack) return false;
    if(!slot.value || lower.local[slot.value]->kind != Value::Alloc) return false;
    if(!isMemoryType(lower.global, slot.type)) return false;

    // Only a shape whose fields are a flat list, which is what a single Field projection can name.
    auto content = slot.type;
    if(lower.global[content]->kind == Type::Record) {
        auto record = (RecordType*)lower.global[content];
        if(record->layout != RecordType::Single || record->constructors.isEmpty()) return false;

        content = record->constructors.get(lower.global, 0).content;
    }

    if(!content || lower.global[content]->kind != Type::Tup) return false;

    for(auto field: ((TupType*)lower.global[content])->fields.contents(lower.global)) {
        if(isMemoryType(lower.global, field.type)) return false;
    }

    // Every use has to be one this translation can rewrite, and all of them in one block.
    ModulePtr<Block> only = nullptr;

    for(auto user: lower.local[slot.value]->uses(lower.local)) {
        auto& instruction = *lower.local[user];

        if(!only) only = instruction.block;
        else if(only != instruction.block) return false;

        Place place;

        switch(instruction.kind) {
            case Value::Init: place = ((InstInit&)instruction).place; break;
            case Value::LoadPlace: place = ((InstLoadPlace&)instruction).place; break;
            default: return false;
        }

        if(place.root != PlaceRoot::Local || place.local != index) return false;

        auto path = place.projections;
        auto steps = path.contents(lower.local);
        auto fields = 0u;

        for(auto projection: steps) {
            if(projection.kind == ProjectionKind::Downcast) continue;
            if(projection.kind != ProjectionKind::Field) return false;
            fields++;
        }

        if(fields != 1) return false;
    }

    return only != nullptr;
}

// The field one place names, for a local this pass decided to scalarize. Only ever asked of a
// place scalarizable() already accepted, so the path is known to be one Field.
U16 scalarField(LowerContext& lower, const Place& place) {
    auto path = place.projections;

    for(auto projection: path.contents(lower.local)) {
        if(projection.kind == ProjectionKind::Field) return projection.index;
    }

    return 0;
}

bool isScalarPlace(LowerContext& lower, const Place& place) {
    return place.root == PlaceRoot::Local && place.local < lower.scalars.size() &&
           lower.scalars[place.local].size() > 0;
}

void prepareScalars(LowerContext& lower, ModulePtr<Function> pointer, Function& function) {
    lower.scalars.clear();
    for(Size i = 0; i < function.localCount(); i++) lower.scalars.push(Array<LowerPtr<LowerValue>>());

    if(!lower.from.ownership) return;

    auto found = lower.from.ownership->functions.get(U32(pointer));
    if(!found) return;

    auto& ownership = found.unwrap();

    for(U32 i = 0; i < function.localCount(); i++) {
        if(!scalarizable(lower, function, i, ownership)) continue;

        auto content = function.localAt(lower.local, i).type;
        if(lower.global[content]->kind == Type::Record) {
            content = ((RecordType*)lower.global[content])->constructors.get(lower.global, 0).content;
        }

        auto count = ((TupType*)lower.global[content])->fields.size();
        for(Size f = 0; f < count; f++) lower.scalars[i].push(nullptr);
    }
}

/*
 * How big a closure header is, checked rather than believed.
 *
 * The code generator places a header's bytes immediately in front of an entry point, and the
 * teardown finds them by subtracting the size of the tuple closureHeaderPlaceType describes - so
 * those two numbers are the same number arrived at two ways, the table layout and the ordinary
 * struct layout. A target whose struct rule disagreed with its table rule would emit a teardown
 * reading from in front of the header, which is the kind of thing that shows up as a crash in
 * unrelated code long afterwards.
 *
 * Checked here because here is the only place both exist. Resolve states the slots and has no
 * offsets; the JS backend has neither, and attaches its header to the code word rather than placing
 * it at a distance, so there is nothing for it to get wrong.
 *
 * The per-slot offsets are no longer checked, and the descriptor is not checked at all: nothing
 * reads either table through a tuple any more. A slot is four bytes and self-relative on this target
 * and an array element on the other, and neither is a field of anything - reading one is
 * InstTableSlot, which asks for the slot rather than describing the bytes. What is left here is the
 * one fact that is still two facts.
 */
static void checkTableTypes(LowerContext& lower) {
    auto root = lower.from.root;
    if(!root) return;

    auto header = closureHeaderPlaceType(*root);
    assertTrue(lower.repr.sizeOf(header) == tableSize(ClosureHeaderFields::kCount));
}

/*
 * A phi, in the two halves its alternatives force it into.
 *
 * `createPhi` builds it detached and registers its result, because an alternative arriving over a
 * back edge is produced by a block this walk has not reached yet - and `mappedValue` is an assertion
 * rather than a lazy lookup for everything that is not a constant. Registering the result first is
 * also what lets the loop body use the phi it is producing an alternative for.
 *
 * `fillPhi` runs once the whole function is lowered, and `addInst` is deliberately part of it:
 * adding an instruction is what records its uses, so the alternatives have to exist by then.
 */
static LowerInstPhi* createPhi(LowerContext& lower, ModulePtr<InstPhi> pointer) {
    auto& phi = *lower.local[pointer];

    // Alternatives that agree on producing nothing join to nothing. A phi of unit type has no
    // lowered type to be built at and no input any block ever named, and every use of it is a use
    // that leaves the position out - see lowerArgExists.
    if(isUnit(lower.global, phi.type)) return nullptr;

    auto count = phi.inputs.size();
    auto storage = lower.to.arena.alloc(
        sizeof(LowerInstPhi) +
        sizeof(LowerPtr<LowerValue>) * count +
        sizeof(LowerPtr<LowerBlock>) * count);

    auto result = new (storage) LowerInstPhi(phi.name, lowerType(lower.global, phi.type));
    result->source = phi.source;
    result->usedCount = count;

    lower.values.add((ModulePtr<Value>)pointer, result->created().ptr - lower.lower);
    return result;
}

static void fillPhi(LowerContext& lower, LowerBlock& block, ModulePtr<InstPhi> pointer,
                    LowerInstPhi* result) {
    if(!result) return;

    auto& phi = *lower.local[pointer];

    Size index = 0;
    for(auto input: phi.inputs.contents(lower.local)) {
        result->used()[index] = mappedValue(lower, input.value);
        result->sources()[index] = lower.blocks.getValue(input.block).unwrap();
        index++;
    }

    block.addInst(lower.lower, result);
}

/*
 * A name no function in this module has yet, which for a generated one is not the name it arrived
 * with.
 *
 * `derivedName` builds a symbol out of a type's *printed form*, and two distinct types can print
 * alike: a lambda's environment tuple is minted fresh rather than interned (see resolveLambda), so
 * two lambdas capturing identical shapes have two types, two derived teardowns, and one name for
 * both. Source can collide the same way through two modules with the same qualified path.
 *
 * A LowerModule holds its functions in a map keyed by name, so a collision there is not two
 * functions sharing a label - it is *one function with two bodies*. `addFunction` finds the existing
 * entry, the second lowering appends its arguments and its blocks to that one, and what reaches the
 * backend has two parameter lists and two entry blocks with nothing able to reach the second. x64's
 * `orderBlocks` asserts on it; a build with assertions off emits it.
 *
 * The tail is only a tail, not a mangling: these are linker symbols, and two of them may not be the
 * same string whatever produced them. The counter is per base name, so a name that never collides
 * never grows one - the same policy `uniqueName` applies on the JS side, where the problem showed up
 * first because that target sanitizes every name and therefore had to answer this already.
 */
static StringId uniqueFunctionName(LowerContext& lower, StringId name) {
    if(!lower.to.functions.get(name)) return name;

    auto base = lower.context.findName(name);

    for(U32 suffix = 1; ; suffix++) {
        StringBuilder text;
        text << base << '$';
        show(suffix, text);

        auto candidate = builtName(lower.context, text);
        if(!lower.to.functions.get(candidate)) return candidate;
    }
}

// Lowering covers the whole program: a call from the root module into Core has to reach a
// LowerFunction, and the two live in the same arena precisely so that it can.
Ptr<LowerModule> lowerProgram(Context& context, Program& program) {
    // The optimizer, against this target - see compiler/opt. Here rather than in the driver because
    // this function is what every native consumer reaches for, the fixture runner included, and an
    // optimization the tests do not see is one the tests do not check.
    {
        StageScope stage(CompileStage::Optimize);
        optimizeProgram(context, program, nativeReprTarget());
    }

    auto result = Ptr<LowerModule>(new LowerModule(8 * 1024 * 1024));
    ReprTable repr(*program.types, nativeReprTarget());

    LowerContext lower {
        context, program, *result, *program.types, *program.arena, *result->arena, repr
    };

    checkTableTypes(lower);

    /*
     * The compiler-built tables, kept until every function exists.
     *
     * Two things are deferred rather than done as each global is reached, and for the same reason: a
     * TypeDesc names teardown glue that has not been generated when the table itself is built, and
     * nothing has an address at all until the module is placed. So a slot naming a function becomes
     * a relocation, and the relocation is translated in a second pass over these.
     *
     * `offsets` is where each slot of the table landed, which is what turns a slot number into a
     * relocation offset. It is kept per table rather than recomputed because the layout rule is the
     * materializer's and asking it twice would be two chances to disagree.
     */
    struct RelocatedGlobal {
        ModulePtr<Global> source;
        LowerPtr<LowerGlobal> target;
        PackOffsets offsets;
    };

    Array<RelocatedGlobal> relocated;

    /*
     * The addresses inside a *source* constant, on the same terms as a table's.
     *
     * One list for the whole module rather than one per global, because a source constant's
     * relocations are already at byte offsets when they are found - a table's are at slot numbers,
     * and `offsets` is what turns those into offsets afterwards. What both still have to wait for is
     * the global they name existing, which is why neither is translated where it is produced.
     */
    struct ConstantAddress {
        LowerPtr<LowerGlobal> target;
        ConstRelocation address;
    };

    Array<ConstantAddress> constantAddresses;

    // The bytes of one global, wherever they end up going.
    auto lowerGlobal = [&](ModulePtr<Global> globalPointer) {
        auto source = lower.local[globalPointer];
        auto target = new (result->arena) LowerGlobal(source->name);

        // What is written rather than what may be assigned - see Global::isWritten, and
        // LowerGlobal::mut, which is the question this side of the wall is asking.
        target->mut = source->isWritten();

        PackOffsets offsets;

        if(source->isTable) {
            /*
             * A compiler-built table, laid out here rather than where it was described.
             *
             * This is the whole of what the structured form bought: resolve said which slot holds
             * the size and which holds the drop, and *this* target decides that an address is eight
             * bytes little-endian and that five words are followed by three of them. The JS backend
             * reads the same slots and never produces bytes at all.
             */
            SmallArray<TableSlot, 8> slots;
            for(auto slot: source->table.contents(lower.local)) slots.push(slot);

            target->initialContents = materializeTable(result->arena, lower.repr,
                                                       toBuffer(slots), offsets);
        } else if(source->literalBytes.length) {
            // A string literal's bytes, copied rather than described - see Global::literalBytes.
            // Resolve already encoded them into the target's native unit, so there is no layout
            // question left for this stage to answer.
            auto size = source->literalBytes.length;
            target->initialContents = ByteBuffer((Byte*)result->arena.alloc(size), size);
            copy(source->literalBytes.ptr, target->initialContents.ptr, size);
        } else {
            /*
             * The global's constant, laid out with the same Repr every other value of its type is -
             * see repr/constant.h, which is where that walk lives for the same reason a table's
             * does. A `dynamic` global has no constant and starts at the zero of its type, which is
             * the buffer this leaves untouched.
             */
            auto size = typeSize(lower, source->type);
            target->initialContents = ByteBuffer((Byte*)result->arena.alloc(size), size);
            set(target->initialContents.ptr, size, 0);

            if(source->initial) {
                Array<ConstRelocation> addresses;

                // False is a compiler bug rather than a program's - `declareGlobal` refuses the one
                // constant that has no static form. Reported as an internal error rather than
                // asserted, because a wrong global is worse than a missing one either way.
                if(!materializeConstant(lower.repr, lower.local, source->initial,
                                        target->initialContents, addresses)) {
                    context.diagnostics.error("internal: the constant of %@ has no form in this target's layout"_v,
                                              source->source, context.findName(source->name));
                }

                for(auto& relocation: addresses) {
                    constantAddresses.push(ConstantAddress { target - lower.lower, relocation });
                }
            }
        }

        relocated.push(RelocatedGlobal { globalPointer, target - lower.lower, ::move(offsets) });
        return target;
    };

    // Globals come first: a function's very first instruction may take the address of one, and
    // the lower module resolves that by name.
    for(auto module: program.modules) {
        for(auto globalPointer: module->globalOrder.contents(lower.local)) {
            auto source = lower.local[globalPointer];
            if(!source->used) continue;

            // A closure header is emitted in front of the function it belongs to rather than into
            // the module's data, so it is not one of these - see LowerFunction::prefix.
            if(source->prefixOf) continue;

            auto target = lowerGlobal(globalPointer);

            // Carried across rather than looked up by name later: what the backend needs is the
            // lowered global, and this is the one place both halves of it are in hand.
            if(globalPointer == lower.from.imageAnchor) result->imageAnchor = target - lower.lower;

            /*
             * The list holds exactly the map's values, in the order the names first appeared.
             *
             * Two modules can reach the same name - a string literal is numbered per module, so
             * `string$0` is made by every module that has one - and the map has always kept the last
             * of those and emitted it once. Pushing unconditionally emitted it twice, which is what
             * `@string$0` appearing in Format.yana's dump under two different offsets was.
             */
            auto entry = result->globals.add(source->name);
            auto pointer = target - lower.lower;

            if(entry.existed) {
                for(auto& existing: result->globalOrder) {
                    if(existing == *entry.value) {
                        existing = pointer;
                        break;
                    }
                }
            } else {
                result->globalOrder.push(pointer);
            }

            *entry.value = pointer;
        }
    }

    Array<ModulePtr<Function>> emitted;
    for(auto module: program.modules) {
        for(auto functionPointer: module->functionOrder.contents(lower.local)) {
            if(lower.local[functionPointer]->signature) continue;

            // A generic function has machine code of its own only when something takes the erased
            // path to it. Where every call site specialized, its instantiations are what reaches the
            // backend and the generic body is a compile-time artifact.
            if(lower.local[functionPointer]->gen && !lower.local[functionPointer]->genericallyUsed) continue;
            if(!lower.local[functionPointer]->used) continue;
            emitted.push(functionPointer);
        }
    }

    for(auto functionPointer: emitted) {
        auto function = lower.local[functionPointer];

        auto target = result->addFunction(uniqueFunctionName(lower, function->name));
        target->source = function->source;

        // Which of them the program starts at, recorded under the name it ended up with rather than
        // the one it was resolved under - see LowerModule::entry.
        if(functionPointer == program.entry) result->entry = target->name;

        if(!isUnit(lower.global, function->returnType) && !isMemoryType(lower.global, function->returnType)) {
            target->returnTypes.push(result->arena, lowerType(lower.global, function->returnType));
        }

        // A lifted lambda's closure header travels with it, because where those bytes go is stated
        // relative to this function's entry point and nowhere else - and is left out entirely where
        // nothing computes that address any more, which is markClosureHeaders' answer.
        if(function->closureHeader && function->closureHeaderRead) {
            target->prefix = lowerGlobal(function->closureHeader) - lower.lower;
        }

        lower.functions.add(functionPointer, target - lower.lower);
    }

    // Now that every function and every global exists, the tables that hold their addresses can say
    // which ones. The addresses themselves are still unknown - that is the loader's half.
    for(auto& entry: relocated) {
        auto source = lower.local[entry.source];
        auto target = lower.lower[entry.target];

        Size index = 0;

        for(auto slot: source->table.contents(lower.local)) {
            auto at = index++;
            if(!isAddressCell(slot.kind)) continue;

            LowerDataRelocation translated;
            translated.offset = entry.offsets[at];
            translated.anchorRelative = true;

            if(auto function = slot.function()) {
                auto found = lower.functions.getValue(function);

                // A table naming a function nothing else reached is what keeps that function alive,
                // so this should not happen; leaving the slot null is still better than pointing it
                // at the wrong thing.
                if(!found) continue;
                translated.function = found.unwrap();
            } else if(auto global_ = slot.global()) {
                auto found = result->globals.getValue(lower.local[global_]->name);
                if(!found) continue;
                translated.global = found.unwrap();
            } else {
                // A deliberately empty address slot - "nothing to do", which is zeroes and no
                // relocation. See TableCell.
                continue;
            }

            target->relocations.push(result->arena, translated);
        }
    }

    // And the addresses a source constant holds, which is the same wait for the same reason: the
    // bytes of a string literal are a global of their own, and the run pointing at them cannot say
    // which until that global exists.
    for(auto& entry: constantAddresses) {
        auto found = result->globals.getValue(lower.local[entry.address.global]->name);
        if(!found) continue;

        LowerDataRelocation translated;
        translated.offset = entry.address.offset;
        translated.global = found.unwrap();

        lower.lower[entry.target]->relocations.push(result->arena, translated);
    }

    for(auto functionPointer: emitted) {
        auto function = lower.local[functionPointer];
        auto target = lower.lower[lower.functions.getValue(functionPointer).unwrap()];

        /*
         * The two hidden parameters, in the order a caller writes them.
         *
         * The environment comes first because it is the one every generic function has, whatever
         * its signature: Implementation-Generics.md part 8's "every unspecialized generic function
         * receives `GenEnv*` as a hidden first argument". The result storage follows, for the same
         * reason it does in a concrete function - a value of unknown size cannot come back in a
         * register, so the caller says where to put it.
         */
        lower.genEnv = nullptr;
        lower.genContext = functionGen(lower.global, *function);
        lower.genModule = function->module;

        if(lower.genContext) {
            auto envArg = target->addArg(lower.lower, lower.context.addUnqualifiedName("genEnv", 6),
                                         LowerType::Pointer);
            lower.genEnv = &envArg->result - lower.lower;
        }

        // An aggregate result is returned through storage the caller passes in, so it becomes a
        // leading pointer argument that every `ret` in the function copies into.
        if(isMemoryType(lower.global, function->returnType)) {
            auto returnPlace = target->addArg(lower.lower, StringId(), LowerType::Pointer);
            lower.returnPlaces.add(functionPointer, &returnPlace->result - lower.lower);
        }

        for(auto argPointer: function->args.contents(lower.local)) {
            auto arg = lower.local[argPointer];

            // A parameter that carries nothing is not received - see lowerArgExists. Nothing maps
            // it either, so a body that names it has to be reaching it through a form that already
            // knows a unit value is not a value: a `ret` reads it off the return type, and every
            // call leaves the position out.
            if(!lowerArgExists(lower.global, arg->type, arg->isMutableBorrow())) continue;

            // A `&` parameter arrives as the address of the caller's storage whatever it holds, so
            // its lower type is a pointer even where the borrowed type is a register-sized scalar.
            // For a memory type the two answers already coincide.
            auto argType = arg->isMutableBorrow() ? LowerType::Pointer : lowerType(lower.global, arg->type);
            auto targetArg = target->addArg(lower.lower, arg->name, argType);
            targetArg->source = arg->source;
            lower.values.add((ModulePtr<Value>)argPointer, &targetArg->result - lower.lower);
        }

        for(auto blockPointer: function->blocks.contents(lower.local)) {
            auto sourceBlock = lower.local[blockPointer];
            auto targetBlock = target->addBlock(lower.lower, sourceBlock->name);
            targetBlock->source = sourceBlock->source;
            lower.blocks.add(blockPointer, targetBlock - lower.lower);
        }

        lower.constantBlock = lower.lower[lower.blocks.getValue(function->blocks.get(lower.local, 0)).unwrap()];
        prepareScalars(lower, functionPointer, *function);

        // Every phi in the function before any of them is filled in, so that an alternative coming
        // back around a loop is a value this walk has already heard of by the time it is asked for.
        SmallArray<LowerInstPhi*, 16> phis;
        for(auto blockPointer: function->blocks.contents(lower.local)) {
            for(auto phi: lower.local[blockPointer]->phis(lower.local)) {
                phis.push(createPhi(lower, phi));
            }
        }

        for(auto blockPointer: function->blocks.contents(lower.local)) {
            auto sourceBlock = lower.local[blockPointer];
            auto targetBlock = lower.lower[lower.blocks.getValue(blockPointer).unwrap()];

            for(auto instruction: sourceBlock->instructions(lower.local)) {
                lowerInstruction(lower, *targetBlock, instruction);
            }

            if(sourceBlock->terminator()) {
                lowerTerminator(lower, *targetBlock, sourceBlock->terminator());
            }
        }

        Size phiIndex = 0;
        for(auto blockPointer: function->blocks.contents(lower.local)) {
            auto targetBlock = lower.lower[lower.blocks.getValue(blockPointer).unwrap()];

            for(auto phi: lower.local[blockPointer]->phis(lower.local)) {
                fillPhi(lower, *targetBlock, phi, phis[phiIndex++]);
            }
        }

        // Every local got storage on the way here, because a place is an address and that is the only
        // shape this translation has. Which of those slots actually needed memory is a question about
        // the finished IR rather than about the source, so it is asked now - see lower_promote.h, and
        // isDirectType in resolve/type.h for why it is not asked any earlier.
        promoteStackSlots(lower.lower, *target);

        // What promotion turned into arithmetic over literals - a local's whole initial value is
        // assembled out of a load of the storage it is about to stop being. See lower_fold.h.
        foldFunctionConstants(lower.lower, lower.to, *target);

        // Then the operations that are still divisions and multiplications, but by a number now
        // known - which is a question that could only be asked once the fold above had answered
        // *which* operands are literals. See lower_strength.h, and note that it emits arithmetic of
        // its own, so the fold runs again behind it before the dead immediates are swept.
        strengthReduceFunction(lower.lower, lower.to, *target);
        foldFunctionConstants(lower.lower, lower.to, *target);

        // And the multiply that is left because its other operand is the loop's own counter, which
        // is not a strength reduction of one operation into another but of a whole recurrence - see
        // lower_induction.h. After the fold above, since what it needs to read is the stride as a
        // number; it emits an immediate of its own, so the fold and the sweep run behind it.
        reduceInductionVariables(lower.lower, lower.to, *target);
        foldFunctionConstants(lower.lower, lower.to, *target);
        removeDeadConstants(lower.lower, lower.to.arena, *target);
    }

    return result;
}
