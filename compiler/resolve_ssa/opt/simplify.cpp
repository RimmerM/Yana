#include "pass.h"

/*
 * One walk, five rewrites, driven by the use lists - README.md's section B.
 *
 * The shipping stage (`compiler/resolve`, deleted in J173) ran constant folding, the algebraic
 * identities, the `Bool` bitwise rewrite, CSE and dead-value removal as five separate passes and
 * repeated the group up to eight times, because its IR had no use-list worklist and each of the
 * five could only hear about the others' work by being walked again. Every one of the five is local
 * to an instruction and its operands, and `ssa::Value` already carries `uses`, so here they are one
 * pass: fold an operand and its readers go back on the queue, erase an instruction and its
 * operands' definers do.
 *
 * The five stay in separate functions and the fold table stays in a file of its own, because the
 * table is large and is not a walk. What is here is the walk, the queue, and the one thing all five
 * have in common - the ownership rule below.
 *
 * ## The ownership rule, which is what makes this the safe tier to do any of it in
 *
 * Every rewrite here either **erases** an instruction or **unifies** two values, and the linear rule
 * is what decides which instructions may be either. `isRemovable` is the whole of it - defined in
 * `opt/cse.cpp`, because the walk of the dominator tree there asks the same question and two
 * spellings of it is one of them learning about a new convention and the other not:
 *
 *  - only a **`kSsaPure`** kind, which is the column that says an instruction may be recomputed or
 *    not computed at all;
 *  - **no operand that is tracked** - `Owned` or `Affine` - and no operand named by a `Consume` use.
 *    Erasing an instruction drops every use it records, so a consumption that went away would leave
 *    an owner unconsumed on a path that had consumed it, which is the *verifier's* first rule and
 *    would be reported at this pass's checkpoint. No pure kind declares `kSsaConsumes` today, so this
 *    guard fires on nothing; it is written because the two columns are independent and a kind added
 *    to both would arrive here silently;
 *  - results that are **`Trivial` or `Address`** and nothing else. A `Guaranteed` value is the case
 *    this declines rather than reasons about: what a borrow is checked against is its *loan*, and
 *    unifying two borrows on different loans moves an endpoint past a use with nothing in the IR
 *    changed to say so. `Editor::replaceAllUses` refuses it, and refusing it here is refusing it
 *    before the assertion rather than at it.
 *
 * A `Const` is never duplicated by anything here and could not be: `Editor::addConst` asserts its
 * type is not linear, so a constant's result is `Trivial` by construction and there is no owner for
 * a second copy of one to be a second copy *of*.
 *
 * ## And the thing a fold must not make
 *
 * A `{}` is **not a value at this tier**. `Conversion::convertNode` drops a unit constant on the
 * floor and `addCall` takes a `hasResult` flag for the same reason - a value of no bits is one
 * nothing below this can map - so a fold that answered a unit would be inventing the one thing the
 * conversion is careful never to produce. Nothing here can: every constant `fold.cpp` makes is an
 * integer, a float or a constructor index, and every identity answers with an operand that was
 * already standing there.
 */

namespace ssa {

namespace {

struct Simplifier {
    OptContext& opt;

    /*
     * The queue, and the bit that keeps an instruction from being on it twice.
     *
     * A head index rather than a stack, so that the seeding walk's order survives into the first
     * pass over the body: a fold reaches an operand before its reader, which is what makes a chain of
     * arithmetic collapse in one sweep rather than in one sweep per link.
     *
     * Both are indexed by `InstId`, which is dense and **grows** while this runs - every constant a
     * fold makes is a new instruction - so `marked` is pushed into rather than sized once.
     */
    Array<U32> queue;
    Array<U8> marked;
    Size head = 0;

    void mark(Size index, U8 value) {
        while(marked.size() <= index) marked.push(0);
        marked[index] = value;
    }

    void push(Inst* inst) {
        if(!inst || !inst->block) return;

        auto index = Size(inst->id.index);
        if(index < marked.size() && marked[index]) return;

        mark(index, 1);
        queue.push(U32(index));
    }

    void push(SsaPtr<Inst> pointer) {
        if(pointer) push(opt.base[pointer]);
    }

    // Everything that reads one value, which is what a rewrite of the instruction defining it has
    // just changed the operands of.
    void pushUsers(Value& value) {
        for(auto user: value.uses(opt.base)) push(user);
    }

    // And the other direction: what this instruction read, whose last reader an erase may have been.
    void pushOperands(Inst& inst) {
        forEachUse(opt.base, inst, [&](Use used) {
            if(used.isSet()) push(opt.base[used.value()]->definer);
        });
    }

    /*
     * The three questions all five rewrites ask - `opt/cse.cpp`, where each is defined and where the
     * walk of the dominator tree asks the same three.
     *
     * Wrappers rather than calls at every site, because the context is the only argument they add
     * and this file asks them a dozen times. `isRemovable` is the ownership rule and the file
     * comment above is where its three clauses are argued; `isUnifiable` is whether one value may
     * stand for another; `soleResult` is the one result a substitution needs.
     */
    bool isRemovable(Inst& inst) { return ssa::isRemovable(opt, inst); }
    bool isUnifiable(Value& from, Value& to) { return ssa::isUnifiable(from, to); }
    Value* soleResult(Inst& inst) { return ssa::soleResult(opt, inst); }

    // The readers of one value pointed at another and the instruction taken out, in that order -
    // `eraseInst` asserts that nothing reads what it removes.
    void replaceAndErase(Inst& inst, Value& result, Value& with) {
        pushUsers(result);

        auto editor = opt.ir();
        editor.replaceAllUses(result, with);
        editor.eraseInst(inst);

        pushOperands(inst);
        push(with.definer);
    }

    /*
     * The constants and the algebraic identities - `opt/fold.cpp`, which is the table.
     *
     * Answers a value and never a rewrite, so the whole of what is here is the ownership guard and
     * the erase. A fold that made a constant left it standing immediately above this instruction,
     * which is where `Editor::setInsertBefore` put it and is what keeps the replacement dominating
     * every reader of what it replaces.
     */
    bool tryFold(Inst& inst) {
        if(!isRemovable(inst)) return false;

        auto result = soleResult(inst);
        if(!result) return false;

        auto folded = foldInst(opt, inst);
        if(!folded || folded == result || !isUnifiable(*result, *folded)) return false;

        replaceAndErase(inst, *result, *folded);
        return true;
    }

    /*
     * A field read out of an aggregate **value** this body built - the shape a record that never
     * reaches storage puts in front of every storage pass.
     *
     * `forwardMemory` answers a load from the store that filled the slot, and that is the whole of
     * what it can see: an `Aggregate` writes no slot, so a record built as a value and then projected
     * is a read with no write above it and the pass declines. The value is right there in the
     * instruction's operand list, though, which is what makes this a peephole rather than a
     * dataflow question - the address chain is two instructions long and both ends are in hand.
     *
     * The **discriminant** is the same statement about the same instruction: which constructor a
     * record built here is, is written on the `Aggregate` and not in any storage, so a projection of
     * it has a constant answer. That is what turns `match` on a `Maybe` a body just made into a
     * decided branch, and it is the rung the sizing of a formatted string stands on - `showBound`
     * answers `Just(20)` and the caller matches on it three instructions later.
     *
     * **The shape that produces it is a flattened adaptor chain.** A `for` over an `iter fn` builds
     * the continuation's environment as one aggregate of borrowed fields and hands the pair to the
     * iterator (`resolve_ssa/convert.cpp`); once `flattenContinuations` has copied both bodies into
     * the consumer, every capture the continuation reads is `load field.addr .x %env` against that
     * aggregate. Without this the record survives to the lowering as a frame object and every
     * capture is a load per iteration - which is the second half of the flattening rather than a
     * separate optimization.
     *
     * Three things are required and each is the same statement from a different side:
     *
     *  - **the record's storage is never written and never escapes.** Every use of the aggregate is
     *    a `field.addr`, and every use of each of those is a `load`. A `fun.value` holding it, a
     *    call handed it, a `store` through it or a `drop` of it all leave, because what would then
     *    be at the field is not what the aggregate put there.
     *  - **the component is borrowed, or it is consumed and owes nothing.** A consuming component's
     *    ownership ends at the aggregate, so pointing a reader below it at the same value is a
     *    second consumption - of a value that has an owner. A component that owes neither a teardown
     *    nor a follow has none, and consuming one twice is consuming it zero times. Every field of a
     *    continuation environment is a borrow, which is why this fires there; the consumed rung is
     *    what reaches a record a body built out of numbers, which is most of them.
     *  - **the two ends are restated rather than unified.** A borrowed field is `T [slot n]` with a
     *    `Guaranteed` convention where the load of its address answers `%T address` - the same
     *    pointer, and two different statements about it (`convert.cpp` makes the pair). `represent`
     *    is what says so, and it is the same restatement the inliner makes at a graft's two ends.
     */
    bool tryAggregateField(Inst& inst) {
        if(inst.kind != Inst::Load || ((InstLoad&)inst).overread || ((InstLoad&)inst).isVolatile) return false;

        auto result = soleResult(inst);
        if(!result) return false;

        /*
         * A read that hands back a **borrow** is declined, and it is `replaceAllUses`' rule rather
         * than a shape this cannot reach: a borrow is checked against its loan, and what would stand
         * in its place is a value rooted where the aggregate's component was rooted. That is a
         * different extent, and moving one is not this rewrite's to do.
         */
        if(result->convention != Convention::Trivial && result->convention != Convention::Address) {
            return false;
        }

        auto address = opt.base[((InstLoad&)inst).address.value()];
        if(!address->definer) return false;

        auto projection = opt.base[address->definer];
        if(projection->kind != Inst::FieldAddress) return false;

        auto& field = (InstFieldAddress&)*projection;
        if(field.property != InstFieldAddress::kNoProperty) return false;

        auto record = opt.base[field.base.value()];
        if(!record->definer) return false;

        auto made = opt.base[record->definer];

        /*
         * **Through the `represent`**, which is §J63 and is the step that makes this rule reach a
         * string literal at all.
         *
         * `"alias"` arrives as `stringLiteral(units, length)` inlined: the two words are built as a
         * `StringView` aggregate and then restated as the `String` occurrence a shared position
         * holds - `mir::UnaryOp::Represent`, which moves no byte and whose lowering answers the
         * operand's address. So `length(s)` on the literal is a field read of an aggregate with one
         * instruction standing between them, and that instruction is not a barrier: the bytes the
         * projection names are the bytes the aggregate built.
         *
         * `field.record` is the record the *occurrence* manifests as (`mir::fieldRecordOf`, which
         * the converter looked the index up through), so across a represent it is already the view
         * record rather than the represented type - `StringView` and not `String`. That it is the
         * record the aggregate built is checked below rather than assumed, because a field index
         * means nothing except against the declaration it indexes.
         */
        auto built = record;
        if(made->kind == Inst::Unary && ((InstUnary&)*made).op == mir::UnaryOp::Represent) {
            built = opt.base[((InstUnary&)*made).value.value()];
            if(!built->definer) return false;

            made = opt.base[built->definer];
        }

        if(made->kind != Inst::Aggregate) return false;
        if(field.record != built->occurrence.type) return false;

        auto& aggregate = (InstAggregate&)*made;
        if(!readOnlyRecord(*built, 0)) return false;

        /*
         * Which constructor's fields the components are, which is the same question the lowering
         * asks of the same instruction - `Inst::Aggregate` in `lower/inst.cpp`, where the walk over
         * the components starts at `arm.first`. The record's fields are one flat list and a
         * constructor owns a range of it (`mir::RecordCon`), so a component's position in the
         * instruction is its field index *minus* that range's start.
         *
         * The read is declined where the projected field is outside the range, and that is not a
         * conservative guard: a field of another arm has nothing standing for it here, because the
         * aggregate built this one. Such a projection is an impossible pattern rather than a shape
         * with an answer this could give.
         */
        auto& declared = *(mir::RecordType*)opt.decl[field.record];
        if(aggregate.constructor >= declared.cons.size()) return false;

        auto arm = declared.cons.get(opt.decl, aggregate.constructor);
        if(aggregate.components.size() != arm.count) return false;

        /*
         * The **discriminant** is answered by which constructor was built, and that number is
         * standing on the instruction.
         *
         * `RecordCon::value` rather than the index, because the value is what is *stored* - a
         * `@value(n)` pins it - and this is the same constant the lowering writes into a tag word
         * (`Discriminant::Word` in `lower/inst.cpp`, from the same `arm.value`). Answering the index
         * would be right on every record nothing pinned and wrong on the ones that matter.
         *
         * Nothing about the record's *layout* is consulted and nothing here needs to be: a folded or
         * bit-tagged tag has no word to load, but what the load would have answered is this number
         * either way, and which bits carry it is a question this fold removes rather than asks.
         */
        if(field.field == InstFieldAddress::kDiscriminant) {
            if(result->convention != Convention::Trivial) return false;
            if(opt.decl[result->occurrence.type]->kind != mir::Type::Int) return false;

            auto tag = makeConstant(opt, inst, result->occurrence.type, U64(arm.value));
            replaceAndErase(inst, *result, *tag);
            return true;
        }

        if(field.field < arm.first || field.field >= U16(arm.first + arm.count)) return false;

        auto component = aggregate.components.get(opt.base, U16(field.field - arm.first));
        auto held = opt.base[component.value()];

        /*
         * A **consumed** component is admitted only where the value it holds owes nothing.
         *
         * The rule this replaces declined every one of them, on the argument that a consuming
         * component's ownership ends at the aggregate so a reader below it would be a second
         * consumption. That is exactly right for a value with an owner and says nothing about one
         * without: `hasTeardownAt` and `isLinearAt` are the two halves of owing something, and a
         * component answering neither is a number that was written into a field. Consuming it twice
         * is consuming it zero times.
         *
         * The occurrence is asked rather than the type, for `pass.h`'s reason - a borrowed field of
         * an owning type owes nothing, and the type alone cannot say so.
         */
        if(component.kind() == UseKind::Consume) {
            if(hasTeardownAt(opt, held->occurrence)) return false;
            if(isLinearAt(opt, held->occurrence)) return false;
        }

        auto editor = opt.ir();
        editor.setInsertBefore(inst);

        auto with = held;
        if(held->occurrence != result->occurrence || held->convention != result->convention) {
            if(isTracked(held->convention)) return false;

            with = editor.resultOf(*editor.addUnary(inst.source, mir::UnaryOp::Represent,
                                                    editor.use(*held, UseKind::Inspect),
                                                    result->occurrence, result->convention));
        }

        replaceAndErase(inst, *result, *with);
        return true;
    }

    /*
     * The aggregate's storage read and nothing else - see above. A linear walk of two short use
     * lists, which is what an aggregate nothing stores into has.
     *
     * A `represent` is a second **name** for the storage rather than a use of it, so it is followed
     * rather than refused, and what has to be read-only is everything reached through it. One level
     * is what the shape needs - a view record is restated once and the restatement is what the field
     * addresses hang off - and the bound is what keeps this a walk of two short lists.
     */
    bool readOnlyRecord(Value& record, U32 depth) {
        for(auto pointer: record.uses(opt.base)) {
            auto user = opt.base[pointer];

            if(user->kind == Inst::Unary && ((InstUnary&)*user).op == mir::UnaryOp::Represent) {
                auto restated = depth < 2 ? soleResult(*user) : nullptr;
                if(!restated || !readOnlyRecord(*restated, depth + 1)) return false;
                continue;
            }

            if(user->kind != Inst::FieldAddress) return false;
            if(((InstFieldAddress&)*user).property != InstFieldAddress::kNoProperty) return false;

            auto projected = soleResult(*user);
            if(!projected) return false;

            for(auto readerPointer: projected->uses(opt.base)) {
                if(opt.base[readerPointer]->kind != Inst::Load) return false;
            }
        }

        return true;
    }

    /*
     * A `Bool` select is a boolean function of two truth values, and every one of those is bitwise.
     *
     * `a && b` reaches this tier as a diamond whose join is a block argument; if-conversion (phase 7)
     * turns that into `select c ? b : False`, and what a backend then makes of it is a conditional
     * move with the constant arm loaded into a register first - three instructions and ten bytes for
     * one `and`. The two arms of a `Bool` select are 0 or 1 by the type's own definition, so a select
     * between them is `and`, `or` or `not` of values the function already has, and all three are
     * `Bitwise(Bool)` members that `lower/inst.cpp` already lowers.
     *
     * **Here rather than in the folder because every rule replaces this instruction with one of a
     * different kind.** That is the division between the two files, and it is not "makes an
     * instruction": `fold.cpp` makes constants, and its one reassociation rule makes a binary of the
     * kind it was already given. What is here is a `select` becoming an `and`. `c ? True : False` is
     * `c`, a value already standing there, so it is `foldSelect`'s and is answered before this is
     * reached.
     *
     * **What produces a `Bool` select is `convertSelects`, and nothing else.** The only other
     * `select` in the corpus is the vector one - `Lanewise.select`, and `min`/`max` expanded out of
     * it - whose condition is a mask and whose result is never a `Bool`, which is what the type test
     * below is for. So this was written in phase 4 and fired on nothing until phase 7 arrived; it now
     * fires seventeen times over the corpus, and `IfConvert.yana` is the fixture that says all four
     * rules are right. Whichever `select` reaches here carries `Inspect` operands, which is
     * `select.cpp`'s doing and is what lets `isRemovable` look at one at all.
     *
     * The **mirror pair** - a false arm of `True`, a true arm of `False` - is declined, and it is the
     * shipping stage's reason: each is the same rewrite over `!c`, which is a second instruction this
     * would have to insert, and whether that is free is a question about the rest of the function
     * rather than about this instruction. It is free where the program already computed `!c` and
     * costs a `not` where it did not, and nothing here can see which.
     *
     * De Morgan is `tryDeMorgan` below, and it is the rewrite this comment used to say was worth
     * doing when a measurement asked for one.
     *
     * The condition is checked against `Bool` as well as the result, because `Select` is also the
     * lane-wise one: a vector select's condition is a `Mask`, its arms are vectors, and neither the
     * arms being 0 or 1 nor `and` meaning what it means here is true of one.
     */
    bool tryBoolean(Inst& inst) {
        if(inst.kind != Inst::Select || !isRemovable(inst)) return false;

        auto& select = (InstSelect&)inst;
        auto result = soleResult(inst);
        if(!result) return false;

        auto boolean = mir::booleanType(opt.program);
        if(result->type() != boolean) return false;

        auto condition = opt.base[select.condition.value()];
        if(condition->type() != boolean) return false;

        auto whenTrue = constantValueOf(opt, opt.base[select.whenTrue.value()]);
        auto whenFalse = constantValueOf(opt, opt.base[select.whenFalse.value()]);

        auto editor = opt.ir();
        editor.setInsertBefore(inst);

        Inst* made = nullptr;

        if(whenTrue && whenFalse && whenTrue.unwrap() == 0 && whenFalse.unwrap() == 1) {
            // `!c`. One `not`, which is `Bitwise(Bool).not` and is what `op.def` says a complement at
            // a `Bool` is - the complement of the *value*, which `lower/inst.cpp` emits as one `xor`
            // where a `Bool`'s shape is known.
            made = editor.addUnary(inst.source, UnaryOp::Not, editor.use(*condition, UseKind::Inspect),
                                   result->occurrence, Convention::Trivial);
        } else if(whenFalse && whenFalse.unwrap() == 0) {
            // `c && b` answers `b` where `c` held and `False` where it did not, which is `c & b`.
            // Neither arm has to be a constant on the other side - what makes the rewrite sound is
            // that both arms are `Bool`, so both are already 0 or 1.
            made = editor.addBinary(inst.source, BinaryOp::And,
                                    editor.use(*condition, UseKind::Inspect), select.whenTrue,
                                    result->occurrence, Convention::Trivial);
        } else if(whenTrue && whenTrue.unwrap() == 1) {
            made = editor.addBinary(inst.source, BinaryOp::Or,
                                    editor.use(*condition, UseKind::Inspect), select.whenFalse,
                                    result->occurrence, Convention::Trivial);
        }

        if(!made) return false;

        replaceAndErase(inst, *result, *editor.resultOf(*made));
        push(made);
        return true;
    }

    /*
     * The operand of a complement, or null where this value is not one.
     *
     * One spelling, `UnaryOp::Not`, where the shipping stage's version read two. Over there `!c` at
     * a `Bool` could also arrive as `xor c, 1`, because its `reduceBooleanOperations` *built* that
     * form; here the same rewrite builds `not` (see `tryBoolean` above, whose first rule is the one
     * that would produce it), `op.def` gives `Bitwise(Bool).not` the same `Not`, and
     * `lower/inst.cpp` is where a `Bool`'s complement becomes an `xor` against one. So the second
     * spelling does not exist at this tier and reading for it would be reading for a shape nothing
     * writes.
     */
    Value* complementOf(Use used) {
        if(!used.isSet()) return nullptr;

        auto value = opt.base[used.value()];
        if(!value->definer) return nullptr;

        auto definer = opt.base[value->definer];
        if(definer->kind != Inst::Unary || ((InstUnary&)*definer).op != UnaryOp::Not) return nullptr;

        // Read once, which is what makes this a shrink rather than a copy: a complement two things
        // read stays where it is and the combined one is an instruction on top of it.
        if(value->useCount() != 1) return nullptr;

        auto operand = ((InstUnary&)*definer).value;
        if(!operand.isSet()) return nullptr;

        return opt.base[operand.value()];
    }

    /*
     * De Morgan: `!a & !b` is `!(a | b)`, and `!a | !b` is `!(a & b)`.
     *
     * Three instructions become two, and the one that is left is a complement in the position a
     * branch can absorb - `negatedBranchCondition` in `lower/lower_fold.cpp` reads a `je` whose
     * condition is a complement and inverts the arms instead. That second half is why the shipping
     * stage's own comment argued the rewrite was worth having and this file's argued it was not: the
     * lower tier sees the same shape whichever way this stage spells it, so what is actually bought
     * is the *instruction*, and that is a count rather than an argument. `IfConvert.yana`'s `negBoth`
     * is the shape and F's phase 16 note is the number.
     *
     * **Both complements read once.** Otherwise the two `not`s stay standing and this adds a third
     * instruction on top of them; `tryDead` is what removes them once their last reader is this
     * one. The same spelling on both sides is therefore automatic here, where the shipping stage
     * had to ask - it read two spellings of a complement and had to rebuild the one it took apart.
     *
     * One type throughout, asked rather than assumed: `and` and `or` are `Closed` in `op.def` so a
     * combination is at its operands' type, but a complement's operand and result are the same type
     * only because `not` is `Wraps`, and the two sides of the combination may be at a type the
     * result is not (a `Bool` combined out of `@bits` refinements is the shape). What the rewrite
     * rebuilds is one `and`/`or` at the *operands*' type, so all four have to be that one type.
     */
    bool tryDeMorgan(Inst& inst) {
        if(inst.kind != Inst::Binary || !isRemovable(inst)) return false;

        auto& binary = (InstBinary&)inst;
        if(binary.op != BinaryOp::And && binary.op != BinaryOp::Or) return false;

        auto result = soleResult(inst);
        if(!result) return false;

        auto left = complementOf(binary.lhs);
        auto right = complementOf(binary.rhs);
        if(!left || !right) return false;

        auto type = result->type();
        if(left->type() != type || right->type() != type) return false;
        if(opt.base[binary.lhs.value()]->type() != type) return false;
        if(opt.base[binary.rhs.value()]->type() != type) return false;

        auto editor = opt.ir();
        editor.setInsertBefore(inst);

        auto combined = binary.op == BinaryOp::And ? BinaryOp::Or : BinaryOp::And;
        auto inner = editor.addBinary(inst.source, combined, editor.use(*left, UseKind::Inspect),
                                      editor.use(*right, UseKind::Inspect), result->occurrence,
                                      Convention::Trivial);

        auto outer = editor.addUnary(inst.source, UnaryOp::Not,
                                     editor.use(*editor.resultOf(*inner), UseKind::Inspect),
                                     result->occurrence, Convention::Trivial);

        replaceAndErase(inst, *result, *editor.resultOf(*outer));

        push(inner);
        push(outer);
        return true;
    }

    // Whether two pure instructions compute the same thing, and whether one is spelled at its use
    // rather than computed - `opt/cse.cpp`, which is where both are defined and argued. The
    // block-local CSE below and the walk of the dominator tree there are one rewrite at two scopes,
    // so they are one statement of what "the same" means.
    bool sameComputation(Inst& a, Inst& b) { return ssa::sameComputation(opt, a, b); }
    bool materializedAtUse(Inst& inst) { return ssa::materializedAtUse(opt, inst); }

    /*
     * Block-local pure CSE: the second of two identical computations answered by the first.
     *
     * **Block-local, deliberately.** `cse` in `opt/cse.cpp` is the one that walks the dominator tree,
     * and this is not that pass with a smaller scope: it is the case a worklist over uses reaches for
     * free, because within one block "the first one dominates the second" is the instruction order.
     * What the folds above and the inliner below produce is two spellings of one expression standing
     * side by side, and a queue that is already running answers those without a tree or a scope
     * stack. The two share one statement of what "the same computation" is - `sameComputation`,
     * `isRemovable`, `isUnifiable` and `materializedAtUse`, declared in `pass.h`.
     *
     * Within one block, "the first one dominates the second" is the instruction order and needs no
     * tree at all - which is the whole reason this fits inside a worklist pass.
     */
    bool tryCommonSubexpression(Inst& inst) {
        if(!isRemovable(inst) || materializedAtUse(inst)) return false;

        auto result = soleResult(inst);
        if(!result) return false;

        auto block = opt.base[inst.block];

        for(auto pointer: block->instructions(opt.base)) {
            auto earlier = opt.base[pointer];
            if(earlier == &inst) break;

            if(!sameComputation(*earlier, inst)) continue;

            auto with = opt.base[earlier->results.get(opt.base, 0)];
            if(!isUnifiable(*result, *with)) continue;

            replaceAndErase(inst, *result, *with);
            return true;
        }

        return false;
    }

    // A pure instruction whose results nothing reads. The last of the five and the one the other four
    // leave work for: a fold points every reader somewhere else, and what it leaves behind is this.
    bool tryDead(Inst& inst) {
        if(!isDiscardableIfUnread(opt, inst)) return false;

        for(auto resultPointer: inst.results.contents(opt.base)) {
            if(opt.base[resultPointer]->useCount() != 0) return false;
        }

        pushOperands(inst);
        opt.ir().eraseInst(inst);
        return true;
    }

    void visit(Inst& inst) {
        // An instruction an earlier rewrite already took out. The table entry stands - `Body::insts`
        // is indexed by `InstId` - so what says it is gone is that it is in no block.
        if(!inst.block) return;

        if(tryFold(inst)) return;
        if(tryAggregateField(inst)) return;
        if(tryBoolean(inst)) return;
        if(tryDeMorgan(inst)) return;
        if(tryCommonSubexpression(inst)) return;
        tryDead(inst);
    }

    void run() {
        for(auto blockPointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[blockPointer];
            if(block->deleted) continue;

            for(auto pointer: block->instructions(opt.base)) push(opt.base[pointer]);
        }

        while(head < queue.size()) {
            auto index = queue[head++];
            mark(index, 0);

            visit(*opt.base[opt.body->insts.get(opt.base, index)]);
        }
    }
};

} // namespace

void simplify(OptContext& opt) {
    Simplifier simplifier { opt };
    simplifier.run();
}

} // namespace ssa
