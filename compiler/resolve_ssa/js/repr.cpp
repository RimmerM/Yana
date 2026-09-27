#include "repr.h"
#include "name.h"
#include "../packing.h"

/*
 * See repr.h. This is the table; `abi.h` is the call convention over it and `convert.cpp` is its one
 * body-level consumer.
 */

namespace jsir {

StringView typedArrayName(TypedArray kind) {
    switch(kind) {
        case TypedArray::Plain:     return "Array"_v;
        case TypedArray::Int8:      return "Int8Array"_v;
        case TypedArray::Uint8:     return "Uint8Array"_v;
        case TypedArray::Int16:     return "Int16Array"_v;
        case TypedArray::Uint16:    return "Uint16Array"_v;
        case TypedArray::Int32:     return "Int32Array"_v;
        case TypedArray::Uint32:    return "Uint32Array"_v;
        case TypedArray::BigInt64:  return "BigInt64Array"_v;
        case TypedArray::BigUint64: return "BigUint64Array"_v;
        case TypedArray::Float32:   return "Float32Array"_v;
        case TypedArray::Float64:   return "Float64Array"_v;
    }

    return "Array"_v;
}

StringView jsRepKindName(JsRepKind kind) {
    switch(kind) {
        case JsRepKind::Void:       return "void"_v;
        case JsRepKind::Number:     return "number"_v;
        case JsRepKind::HostBool:   return "boolean"_v;
        case JsRepKind::BigInt:     return "bigint"_v;
        case JsRepKind::HostString: return "string"_v;
        case JsRepKind::GcObject:   return "object"_v;
        case JsRepKind::HostArray:  return "array"_v;
        case JsRepKind::Nullable:   return "nullable"_v;
        case JsRepKind::Parts:      return "parts"_v;
        case JsRepKind::Cell:       return "cell"_v;
        case JsRepKind::Erased:     return "erased"_v;
    }

    return "?"_v;
}

StringView jsPlaceKindName(JsPlaceKind kind) {
    switch(kind) {
        case JsPlaceKind::Var:      return "var"_v;
        case JsPlaceKind::Cell:     return "cell"_v;
        case JsPlaceKind::Property: return "property"_v;
        case JsPlaceKind::Element:  return "element"_v;
        case JsPlaceKind::Global:   return "global"_v;
        case JsPlaceKind::Value:    return "value"_v;
        case JsPlaceKind::BitRange: return "bits"_v;
    }

    return "?"_v;
}

StringView jsPropertyRoleName(JsPropertyRole role) {
    switch(role) {
        case JsPropertyRole::Field: return "field"_v;
        case JsPropertyRole::Tag:   return "tag"_v;
        case JsPropertyRole::Box:   return "box"_v;
        case JsPropertyRole::Code:  return "code"_v;
        case JsPropertyRole::Env:   return "env"_v;
        case JsPropertyRole::Owner: return "owner"_v;
        case JsPropertyRole::Key:   return "key"_v;
        case JsPropertyRole::StorageUnit: return "unit"_v;
    }

    return "?"_v;
}

StringView jsPassingKindName(JsPassingKind kind) {
    switch(kind) {
        case JsPassingKind::Omitted: return "omitted"_v;
        case JsPassingKind::One:     return "one"_v;
        case JsPassingKind::Cell:    return "cell"_v;
        case JsPassingKind::Parts:   return "parts"_v;
    }

    return "?"_v;
}

void JsTarget::notYet(StringView what, LocationId where) {
    reported = true;
    refused = true;
    context.diagnostics.error("JSIR does not convert %@ yet"_v, where, what);
}

void JsTarget::notYet(StringView what, DeclPtr<mir::Type> type, LocationId where) {
    reported = true;
    refused = true;
    if(!type) return notYet(what, where);

    context.diagnostics.error("JSIR does not convert %@ yet (%@)"_v, where, what,
                              mir::typeText(context, decl, type));
}

void JsTarget::internalError(StringView what, LocationId where) {
    reported = true;
    context.diagnostics.error("internal: JSIR %@"_v, where, what);
}

U8 jsIntBits(const mir::IntType& type) {
    switch(type.target) {
        // A signed 32-bit index, because a host array's length is a `uint32` by specification and
        // nothing wider can be described anyway - `targetIntWidths` for a JavaScript build, and the
        // one place that number is read here.
        case intrinsic::TargetWidth::Word:     return 32;

        // A UTF-16 unit, because that is what a host string is made of.
        case intrinsic::TargetWidth::CodeUnit: return 16;
        case intrinsic::TargetWidth::Fixed:    break;
    }

    return type.bits;
}

/*
 * Structural interning.
 *
 * A linear scan rather than a hash, and the numbers are why: a compilation of the whole fixture
 * corpus reaches a few dozen distinct representations, so a scan is a handful of compares against a
 * dense array and a hash is a function call plus a probe. `JsShape` interning is keyed instead,
 * because a program has one shape per record and that is thousands.
 */
static bool sameRep(JsTarget& target, const JsRep& a, const JsRep& b) {
    if(a.kind != b.kind) return false;

    switch(a.kind) {
        case JsRepKind::Void:
        case JsRepKind::HostString:
        case JsRepKind::HostBool:
        case JsRepKind::Erased:
            return true;
        case JsRepKind::Number:
            return a.domain == b.domain && a.bits == b.bits;
        case JsRepKind::BigInt:
            return a.bits == b.bits && a.domain == b.domain;
        case JsRepKind::GcObject:
            return a.shape == b.shape;
        case JsRepKind::HostArray:
            return a.content == b.content && a.typedArray == b.typedArray && a.length == b.length;
        case JsRepKind::Nullable:
            return a.content == b.content && a.tag == b.tag && a.absentTag == b.absentTag &&
                   a.presentTag == b.presentTag && a.payloadField == b.payloadField;
        case JsRepKind::Cell:
            return a.content == b.content;
        /*
         * Structurally, and not by where the parts happen to sit.
         *
         * Comparing `firstPart` would make interning a no-op for this kind - every ask would push a
         * fresh run into `partList` and then fail to match the identical one already there - and
         * two representation ids for one representation is the edge check of §3.8 comparing two
         * numbers that are never equal. Which is a `Vec(I32, 4)` carried around a loop reported as a
         * mismatch against itself.
         */
        case JsRepKind::Parts: {
            if(a.partCount != b.partCount || a.content != b.content) return false;
            if(a.lanewise != b.lanewise || a.freshEachUse != b.freshEachUse) return false;
            for(U16 i = 0; i < a.partCount; i++) {
                if(target.partList[a.firstPart + i] != target.partList[b.firstPart + i]) return false;
            }

            return true;
        }
    }

    return false;
}

static JsRepId internRep(JsTarget& target, const JsRep& rep) {
    for(U32 i = 0; i < target.reps.size(); i++) {
        if(sameRep(target, target.reps[i], rep)) return JsRepId(i);
    }

    auto id = JsRepId(target.reps.size());
    target.reps.push(rep);
    return id;
}

JsRepId jsHostBoolRep(JsTarget& target) {
    JsRep rep;
    rep.kind = JsRepKind::HostBool;
    return internRep(target, rep);
}

/*
 * A multi-part representation, interned on its parts and its materialized form.
 *
 * The parts go into `JsTarget::partList` before the scan rather than after, because the scan reads
 * them from there - and the run is taken back off again where an equal representation already
 * exists, so the table holds one run per distinct representation and not one per ask.
 */
JsRepId jsPartsRep(JsTarget& target, Buffer<JsRepId> parts, JsRepId materialized, bool lanewise,
                   bool freshEachUse) {
    JsRep rep;
    rep.kind = JsRepKind::Parts;
    rep.content = materialized;
    rep.firstPart = target.partList.size();
    rep.partCount = U16(parts.length);
    rep.lanewise = lanewise;
    rep.freshEachUse = freshEachUse;

    for(auto part: parts) target.partList.push(part);

    for(U32 i = 0; i < target.reps.size(); i++) {
        if(!sameRep(target, target.reps[i], rep)) continue;

        while(target.partList.size() > rep.firstPart) target.partList.pop();
        return JsRepId(i);
    }

    auto id = JsRepId(target.reps.size());
    target.reps.push(rep);
    return id;
}

JsRepId partRepAt(JsTarget& target, JsRepId parts, U32 index) {
    auto& rep = target.rep(parts);
    if(rep.kind != JsRepKind::Parts || index >= rep.partCount) return JsRepId();

    return target.partList[rep.firstPart + index];
}

JsRepId jsStoredRep(JsTarget& target, JsRepId id) {
    if(!id.valid()) return id;

    auto& rep = target.rep(id);
    return rep.kind == JsRepKind::Parts ? rep.content : id;
}

bool jsPartsAreLanes(JsTarget& target, JsRepId id) {
    if(!id.valid()) return false;

    auto& rep = target.rep(id);
    return rep.kind == JsRepKind::Parts && rep.lanewise;
}

JsRepId jsVoidRep(JsTarget& target) {
    JsRep rep;
    rep.kind = JsRepKind::Void;
    return internRep(target, rep);
}

JsRepId jsNumberRep(JsTarget& target, NumberDomain domain, U8 bits) {
    JsRep rep;
    rep.kind = JsRepKind::Number;
    rep.domain = domain;
    rep.bits = bits;
    return internRep(target, rep);
}

static JsRepId jsBigIntRep(JsTarget& target, bool isSigned) {
    JsRep rep;
    rep.kind = JsRepKind::BigInt;
    rep.domain = isSigned ? NumberDomain::Signed : NumberDomain::Unsigned;
    rep.bits = 64;
    return internRep(target, rep);
}

JsRepId jsStringRep(JsTarget& target) {
    JsRep rep;
    rep.kind = JsRepKind::HostString;
    return internRep(target, rep);
}

JsRepId jsErasedRep(JsTarget& target) {
    JsRep rep;
    rep.kind = JsRepKind::Erased;
    return internRep(target, rep);
}

JsRepId jsGenTableRep(JsTarget& target) {
    return jsHostArrayRep(target, jsErasedRep(target));
}

TypedArray typedArrayFor(JsTarget& target, JsRepId element) {
    if(!element.valid()) return TypedArray::Plain;

    auto& rep = target.reps[element.index];
    if(rep.kind == JsRepKind::BigInt) {
        return rep.domain == NumberDomain::Signed ? TypedArray::BigInt64 : TypedArray::BigUint64;
    }

    if(rep.kind != JsRepKind::Number) return TypedArray::Plain;

    switch(rep.domain) {
        case NumberDomain::Float32: return TypedArray::Float32;
        case NumberDomain::Float64: return TypedArray::Float64;

        /*
         * A `Bool` is a `Uint8Array` at one byte per element against eight for a plain row, which is
         * one of the measurements the 0/1 representation was chosen on.
         */
        case NumberDomain::Bool01:  return TypedArray::Uint8;
        case NumberDomain::Signed:
            if(rep.bits <= 8) return TypedArray::Int8;
            if(rep.bits <= 16) return TypedArray::Int16;
            if(rep.bits <= 32) return TypedArray::Int32;
            break;
        case NumberDomain::Unsigned:
            if(rep.bits <= 8) return TypedArray::Uint8;
            if(rep.bits <= 16) return TypedArray::Uint16;
            if(rep.bits <= 32) return TypedArray::Uint32;
            break;
    }

    /*
     * The 33-to-53-bit band has no typed row, which is the host's answer rather than a gap here:
     * there is no `Int53Array`, and putting one of these in a `Float64Array` would round rather than
     * truncate at the top of the range. A plain array holds the value exactly.
     */
    return TypedArray::Plain;
}

U32 jsScalarBytes(JsTarget& target, JsRepId rep) {
    if(!rep.valid()) return 0;

    auto& kind = target.reps[rep.index];
    if(kind.kind == JsRepKind::HostBool) return 1;

    // The band with no typed row is still a scalar and is still eight bytes - `typedArrayFor`
    // answers `Plain` there for a reason about rounding, which is not a reason about width.
    if(kind.kind == JsRepKind::Number && kind.domain != NumberDomain::Float32 &&
       kind.domain != NumberDomain::Float64 && kind.bits > 32) {
        return 8;
    }

    switch(typedArrayFor(target, rep)) {
        case TypedArray::Int8:
        case TypedArray::Uint8:     return 1;
        case TypedArray::Int16:
        case TypedArray::Uint16:    return 2;
        case TypedArray::Int32:
        case TypedArray::Uint32:
        case TypedArray::Float32:   return 4;
        case TypedArray::BigInt64:
        case TypedArray::BigUint64:
        case TypedArray::Float64:   return 8;
        case TypedArray::Plain:     return 0;
    }

    return 0;
}

JsRepId jsHostArrayRep(JsTarget& target, JsRepId element, U32 length, bool typed) {
    JsRep rep;
    rep.kind = JsRepKind::HostArray;
    rep.content = element;
    rep.typedArray = typed ? typedArrayFor(target, element) : TypedArray::Plain;
    rep.length = length;
    return internRep(target, rep);
}

// The argument is in `repr.h`.
bool jsIsItsOwnStorage(JsTarget& target, JsRepId rep) {
    if(!rep.valid()) return true;

    switch(target.rep(rep).kind) {
        case JsRepKind::GcObject:
        case JsRepKind::Void:
        case JsRepKind::Erased:
            return true;
        default:
            return false;
    }
}

JsShapeId jsBoxShape(JsTarget& target, JsRepId content) {
    // One per content representation, so that every box of a number in the program has one hidden
    // class. A scan for `internRep`'s reason: there are a handful of these, not one per record.
    for(U32 i = 0; i < target.shapes.size(); i++) {
        auto& shape = target.shapes[i];
        if(shape.kind == JsShapeKind::Cell && shape.properties.size() == 1 &&
           shape.properties[0].rep == content) {
            return JsShapeId(i);
        }
    }

    JsShape shape;
    shape.kind = JsShapeKind::Cell;
    shape.properties.push(JsProperty {
        jsir::jsInternText(target.context, target.interned, key::box), content, JsPropertyRole::Box
    });

    auto id = JsShapeId(target.shapes.size());
    target.shapes.push(::move(shape));
    return id;
}

JsShapeId jsRefShape(JsTarget& target) {
    for(U32 i = 0; i < target.shapes.size(); i++) {
        if(target.shapes[i].kind == JsShapeKind::Ref) return JsShapeId(i);
    }

    auto erased = jsErasedRep(target);

    JsShape shape;
    shape.kind = JsShapeKind::Ref;
    shape.properties.push(JsProperty {
        jsir::jsInternText(target.context, target.interned, key::owner), erased, JsPropertyRole::Owner
    });
    shape.properties.push(JsProperty {
        jsir::jsInternText(target.context, target.interned, key::index), erased, JsPropertyRole::Key
    });

    auto id = JsShapeId(target.shapes.size());
    target.shapes.push(::move(shape));
    return id;
}

JsShapeId jsFunPairShape(JsTarget& target) {
    for(U32 i = 0; i < target.shapes.size(); i++) {
        if(target.shapes[i].kind == JsShapeKind::FunPair) return JsShapeId(i);
    }

    auto erased = jsErasedRep(target);

    JsShape shape;
    shape.kind = JsShapeKind::FunPair;
    shape.properties.push(JsProperty {
        jsir::jsInternText(target.context, target.interned, key::code), erased, JsPropertyRole::Code
    });
    shape.properties.push(JsProperty {
        jsir::jsInternText(target.context, target.interned, key::env), erased, JsPropertyRole::Env
    });

    auto id = JsShapeId(target.shapes.size());
    target.shapes.push(::move(shape));
    return id;
}

JsRepId carriedRepOf(JsTarget& target, const JsPassing& passing) {
    if(passing.kind == JsPassingKind::Cell) return jsCellRep(target, passing.rep);

    /*
     * A multi-part **result** is the materialized value, because JavaScript returns one thing.
     *
     * §3.7's table has `Parts -> Parts` for an argument and nothing for a result, and this is where
     * that gap is closed: the parts are passed flat at a call and materialized at a `return`, then
     * split again at the call site. The alternative is an out-parameter the callee writes, which is
     * the same allocation moved to the caller plus a second convention for one value - and the old
     * JavaScript backend (deleted with `compiler/resolve` in J173) took this trade for a function
     * pair and for a vector too. §6.5's materialization sinking is what makes a `return f(x)` chain
     * cost one row rather than one per level, and it is a later slice's.
     *
     * A parameter never reaches this: it occupies `positions` slots and each is read with
     * `partRepAt`, which is what `checkCall` and the entry block both do.
     */
    if(passing.kind == JsPassingKind::Parts) return jsStoredRep(target, passing.rep);

    return passing.rep;
}

/*
 * The `{$v: content}` object an escaping scalar local and a boxed global are *stored in*.
 *
 * `jsCellRep` above is the descriptor that *names* one; this is the storage itself, and the two are
 * deliberately different representations - see `JsRepKind::Cell`'s own note, and §3.5.
 */
JsRepId jsBoxRep(JsTarget& target, JsRepId content) {
    JsRep rep;
    rep.kind = JsRepKind::GcObject;
    rep.shape = jsBoxShape(target, content);
    return internRep(target, rep);
}

JsRepId jsCellRep(JsTarget& target, JsRepId content) {
    JsRep rep;
    rep.kind = JsRepKind::Cell;

    /*
     * A cell names *storage*, and storage on this target holds one host value - so the content of a
     * reference to a multi-part value is its materialized form and never its parts.
     *
     * Normalized here rather than at the six call sites, because it is one statement about what a
     * place is: `&Vec(Int)` is a descriptor naming a property whose value is the lane row. A cell
     * whose content was the `Parts` representation would be a place a `load` cannot read - there is
     * no one value of that representation - and the six sites each getting it right is the shape of
     * mistake this tier exists to remove.
     */
    rep.content = jsStoredRep(target, content);
    rep.shape = jsRefShape(target);
    return internRep(target, rep);
}

static bool packableRep(JsTarget& target, JsRepId id, U32& bits, bool& isSigned);

/*
 * Whether a reference to `content` carries a **scale** - the old JavaScript backend's
 * `narrowRefCarriesScale`, asked of the content rather than of the target alone.
 *
 * Only a content that could be a field of a packed word can be named by a range, and that is
 * exactly `packableRep`'s answer, since a range is only ever built over a field that answered yes -
 * so a `&U8` or a `&Int` stays the two-part pair even with both switches on. And nothing carries
 * one while nothing packs, for that backend's reason: a target with no ranges in it has every
 * narrow value at offset zero of the storage it names, so while nothing is packed nothing may pay
 * for the arithmetic.
 */
bool jsRefCarriesScale(JsTarget& target, JsRepId content) {
    if(!target.packFields && !target.scalarizeRecords) return false;

    U32 bits = 0;
    bool isSigned = false;
    return content.valid() && packableRep(target, jsStoredRep(target, content), bits, isSigned);
}

/*
 * A reference passed flat: `(owner, key)`, or `(owner, key, scale)` where the content is narrow.
 *
 * The scale is a float, because it is `2**offset` of a word up to 53 bits wide and a multiplier
 * rather than anything the integer tower should wrap. The materialized form is the `{$o, $k}` pair
 * in both cases, which is the J84 decision the old JavaScript backend did not take: its one-value
 * form was `{$o, $k, $s}` and every reader of a reference read the scale. Here a three-part
 * reference that has to become one value becomes `$bitCell`'s accessor box instead
 * (`codegen/js/bits.cpp`), so a `Cell` - an erased argument, a stored or returned reference, a phi
 * - keeps meaning `$o[$k]` and the erased ABI stays one object whatever the substitution packs.
 */
JsRepId jsCellPartsRep(JsTarget& target, JsRepId content) {
    auto erased = jsErasedRep(target);
    if(jsRefCarriesScale(target, content)) {
        JsRepId parts[3] = { erased, erased, jsNumberRep(target, NumberDomain::Float64, 64) };
        return jsPartsRep(target, Buffer<JsRepId>(parts, 3), jsCellRep(target, content));
    }

    JsRepId parts[2] = { erased, erased };
    return jsPartsRep(target, Buffer<JsRepId>(parts, 2), jsCellRep(target, content));
}

/*
 * The function pair - `{$c, $e}`, and the one place this slice takes the plan's advice in reverse.
 *
 * §3.2 lists an erased function value under `Parts`, and §5.4 says not to build a wrapper whose
 * consumers all accept the flattened form. That is right and it is milestone 4's: flattening one
 * means a JSIR *value* is several JS values, which every block argument, every edge and every
 * argument list then has to expand - and it buys an allocation at a position that is already rare,
 * because a closure is its own type and a call through a lambda the site can see is a direct call
 * (§3.2.1). So here the pair is one `GcObject` of the two-property shape, `Materialize` and `Split`
 * exist for the day it is not, and `JsRepKind::Parts` is declared and unproduced.
 *
 * next: flatten this into `Parts` once `TargetValue` carries several values (§8.2), and measure the
 * allocations removed rather than assuming them.
 *
 * `Type::Iter` comes here too and gets no family of its own: an erased iterator object is the same
 * two words over the same block - the producer's step shim and the state record - which is §3.2.1's
 * decision and the reason `Iter` sits beside `Fun` at every site rather than beside `Record`.
 */
static JsRepId jsFunRep(JsTarget& target) {
    JsRep rep;
    rep.kind = JsRepKind::GcObject;
    rep.shape = jsFunPairShape(target);
    return internRep(target, rep);
}

/*
 * One property key, distinct within its shape.
 *
 * A source field name goes through `jsSanitize` and nothing else where it is unique; where two
 * constructors of one sum declare a field of the same name, the record-wide field index is appended.
 * The index rather than a counter, because it is stable: a shape printed twice prints the same keys,
 * and a fixture golden does not move when a constructor is added above.
 */
static StringId propertyKey(JsTarget& target, mir::RecordType& record, U16 index) {
    auto field = record.fields.get(target.decl, index);

    char buffer[512];
    Size length = 0;

    if(field.name) {
        auto text = target.context.findName(field.name);
        length = jsSanitize(stringView(text), buffer, sizeof(buffer) - 16);

        auto duplicated = false;
        for(U16 other = 0; other < record.fields.size(); other++) {
            if(other == index) continue;
            if(record.fields.get(target.decl, other).name == field.name) duplicated = true;
        }

        if(!duplicated) return jsInternText(target.context, target.interned, StringView { buffer, length });

        buffer[length++] = '$';
        length += show(U64(index), buffer + length, sizeof(buffer) - length);
        return jsInternText(target.context, target.interned, StringView { buffer, length });
    }

    // The anonymous payload form - `Just(a)`'s one field. `f0`, `f1`, ... which is the spelling the
    // old JavaScript backend gave an unnamed field, kept so that its goldens and these read alike.
    buffer[0] = 'f';
    length = 1 + show(U64(index), buffer + 1, sizeof(buffer) - 1);
    return jsInternText(target.context, target.interned, StringView { buffer, length });
}

static bool isUnitType(JsTarget& target, DeclPtr<mir::Type> type) {
    return !type || target.decl[type]->kind == mir::Type::Unit;
}

static bool isStringRecord(JsTarget& target, DeclPtr<mir::Type> type);
static bool isBoolRecord(JsTarget& target, DeclPtr<mir::Type> type);

static DeclPtr<mir::Type> transparentContentAsked(JsTarget& target, DeclPtr<mir::Type> record);

DeclPtr<mir::Type> transparentContentOf(JsTarget& target, DeclPtr<mir::Type> record) {
    if(!record || target.decl[record]->kind != mir::Type::Record) return nullptr;

    // A record already being asked about is a cycle - see `JsTarget::transparentAsking`.
    for(auto asking: target.transparentAsking) {
        if(asking == U32(record.offset)) return nullptr;
    }

    target.transparentAsking.push(U32(record.offset));
    auto content = transparentContentAsked(target, record);
    target.transparentAsking.pop();
    return content;
}

static DeclPtr<mir::Type> transparentContentAsked(JsTarget& target, DeclPtr<mir::Type> record) {

    // A lambda's environment keeps its object - see `JsTarget::environmentRecords`, where the
    // identity argument is written. The one family whose wrapper is not a cost.
    if(target.environmentRecords.contains(U32(record.offset))) return nullptr;

    auto& declared = *(mir::RecordType*)target.decl[record];
    if(declared.layout != mir::RecordLayout::Single || declared.cons.size() != 1) return nullptr;

    auto constructor = declared.cons.get(target.decl, 0);
    if(!constructor.count) return nullptr;

    // The one field the value is, and whatever `@host` fields follow it.
    auto first = declared.fields.get(target.decl, constructor.first);
    if(first.host || first.boxed || isUnitType(target, first.occurrence.type)) return nullptr;

    /*
     * A field holding a **unique loan** is not the value it names, so the record cannot be that
     * value either. What the property holds is the reference - the `{$o, $k}` descriptor where the
     * content needs a cell, the object where it is one - and a transparent record handed on the
     * *content*: a write through the field then landed in a box built around a copy.
     *
     * A lambda's environment is the record that has such a field, and `environmentRecords` normally
     * keeps it an object; it stops doing so once the lambda is inlined everywhere and so unreachable,
     * while the construction and the reads of the captured `&` survive in the body it was inlined
     * into. `let g = (n: Int) -> swapIn(e, n)` with `swapIn` assigning a new array answered the old
     * array on JavaScript, a captured `&s: Int` passed to `bump` the old number, and a captured
     * `&p: Point` was a verifier refusal (J88). Every unique loan rather than only the ones that need
     * a cell, because the object case - the reference *is* the content - would be transparent over a
     * `GcObject`, a combination no other record produces and nothing downstream builds. Asked here
     * rather than by keeping every environment, because a capture that is only read is still free to
     * be the value.
     */
    if(!first.occurrence.contract.isOwned() &&
       first.occurrence.contract.access() == mir::Access::Unique) {
        return nullptr;
    }

    /*
     * And never over a record that is itself an object - one with properties of its own, a
     * niche-folded sum, a flat run of parts or a packed word.
     *
     * Every site that takes a transparent record apart asks the *representation* whether it is an
     * object and only then the type whether it is transparent - the aggregate, the field projection,
     * the constant and the teardown walk. Over a leaf the two questions cannot disagree. Over a
     * record they do: `data W {b: Box}` had `Box`'s representation, so `W {b: b}` was built as a
     * `Box` object with the whole `b` in its `m` and a zero in its `n`, `w.b` projected `Box`'s slot
     * zero, and `w.b.n` was "does not convert a field of this representation". The same
     * held over a sum, a `Maybe(Box)`, a packed `Box` of two `U8`s, a typed row and a tuple, and it
     * held in an inlined-away lambda environment whose one capture is a record. Teaching each of
     * those sites a third case - the object is the content's, the record's field indices are not
     * its slots - was the alternative; it is four sites for a wrapper that saves one object, and the
     * fifth site to be written would have been the next defect. So such a record is an object
     * holding its one field, which every site already builds.
     *
     * Decided on the *type*, recursively, rather than by asking `jsRep` of the content, for
     * `isStringRecord`'s reason below: a record over a string, a `Bool`, an enum, or a record that
     * is itself transparent over a leaf is still a leaf, and none of those needs its content
     * interned to say so. The recursion into an inner record does reach `jsRep` for one case - an
     * inner field that is a loan neither owned nor a view, whose rule interns - which is harmless:
     * it interns the representation the field has anyway.
     */
    auto contentType = first.occurrence.type;

    /*
     * And never over a **function value** or an erased iterator, for the same reason - J145, J144
     * §5. Both are the two-property `FunPair` object (`jsFunRep`), so a transparent `data Handler
     * {run: (Int, Int) -> Int}` *was* a `GcObject`, the field projection took the object path, and
     * `h.run` read the pair's code word as if it were the field - the verifier's "a property read
     * out of something that is not a host object" at the call through it. Declined here rather than
     * taught at the projection, for the reason above: the aggregate, the constant and the teardown
     * walk ask the same question in the same order.
     */
    auto contentKind = target.decl[contentType]->kind;
    if(contentKind == mir::Type::Fun || contentKind == mir::Type::Iter) return nullptr;

    if(target.decl[contentType]->kind == mir::Type::Record &&
       !isStringRecord(target, contentType) && !isBoolRecord(target, contentType) &&
       ((mir::RecordType*)target.decl[contentType])->layout != mir::RecordLayout::Enum &&
       !transparentContentOf(target, contentType)) {
        return nullptr;
    }

    auto hosted = false;
    for(U16 i = 1; i < constructor.count; i++) {
        auto field = declared.fields.get(target.decl, U16(constructor.first + i));
        if(isUnitType(target, field.occurrence.type)) continue;
        if(!field.host) return nullptr;

        hosted = true;
    }

    /*
     * And a **shared** loan only where what the property holds is a primitive - a number or a host
     * string, which a loan of and the value itself are the same host value.
     *
     * A transparent record's representation is its content *type's* (`jsRep` of the bare type,
     * below and in `jsRep`'s record row), while its construction and every read hand on the
     * component at the *field's* occurrence. For a primitive the two cannot differ; for anything
     * else a shared capture holds they do: a captured window `sep: [a]` is
     * `Array(a) [shared slot n view]` and holds the view's parts, and a captured owned array is a
     * loan the SSA reads as an address and then loads through. Read back through the bare type,
     * each was projected as a representation it is not - the inlined `length(sep)` of a lambda
     * capturing a separator window was "does not convert a field of this representation", and so was `length(xs)` over a
     * captured owned local. The same inlined-away environment J88 describes above, one access
     * over. Asked after the other fields, which is a cheaper answer and one that interns nothing.
     * Comparing `fieldRepOf` against `jsRep` was tried first and is not enough, because the
     * owned array's two representations agree and its reads still go through an address the
     * transparent place does not have; declining every shared loan was tried second and put an
     * object back around `BorrowTemporary.yana`'s `Held {value: 'Int}`, which was right as it was.
     */
    if(!first.occurrence.contract.isOwned()) {
        // A view is a string or a record of parts, and the string is the one of them a type names
        // without interning anything - asking `jsRep` of an owner no value here has would put its
        // representation in the table and renumber every golden that prints one.
        if(first.occurrence.contract.form() == mir::AccessForm::View) {
            if(!isStringRecord(target, first.occurrence.type)) return nullptr;
        } else if(!target.rep(jsRep(target, first.occurrence.type, kNullLocation)).isPrimitive()) {
            return nullptr;
        }
    }

    if(!hosted) return first.occurrence.type;

    /*
     * A `@host` field is elidable only where the value field zero holds genuinely has that property
     * - Implementation-Containers.md §14's two rows, and the element is what chooses.
     *
     * A plain host array's `length` *is* its occupancy and assigning it truncates, so `Array(Handle)`
     * is the bare array. A typed row's `length` is its fixed capacity, so `Array(Int)` keeps its
     * stored count and stays an object holding both. One declaration, two layouts, and which one a
     * value has is settled by the same predicate that chooses which array to allocate.
     */
    auto content = jsRep(target, first.occurrence.type, kNullLocation);
    auto& rep = target.reps[content.index];
    if(rep.kind != JsRepKind::HostArray || rep.typedArray != TypedArray::Plain) return nullptr;

    return first.occurrence.type;
}

/*
 * What one **field** of a record holds, which is the occurrence's question and not the type's.
 *
 * A field is the one position whose ownership a shape has to carry, and it is easy to read as the
 * type's: `data env {total: &Int}` holds a *reference* to a number somebody else owns, and the
 * number's representation says nothing about that. What the field holds is `Cell(Number)` - the
 * `{$o, $k}` descriptor - and reading `jsRep(Int)` instead gave the property the number's
 * representation, so a store through the capture wrote into a copy and a construction of the record
 * supplied the wrong thing. A lambda that writes a captured `&` local is where it arrives
 * (`Closure.yana`), and the symptom was the emitter refusing a whole-value write rather than a
 * wrong answer, which is the tier working.
 *
 * `carriedRepOf` over a `JsPassing` is the same statement at a call, and the two agree by
 * construction because both are `jsManifest`'s `needsCell`.
 */
static JsRepId fieldRepOf(JsTarget& target, const mir::Occurrence& occurrence, LocationId where) {
    auto manifest = jsManifest(target, occurrence, where);

    // `jsStoredRep` for `jsCellRep`'s reason read at the other end: a property holds one host value,
    // so a field of vector type holds the lane row and the parts exist only between operations.
    return manifest.needsCell ? jsCellRep(target, manifest.rep)
                              : jsStoredRep(target, manifest.rep);
}

/*
 * Whether a representation can hold `null` already, and therefore cannot be the payload of a fold.
 *
 * §3.2: "nesting is legal only when the inner rep does not already admit the same absent pattern".
 * A `Nullable` is the obvious one; the other three are the ones whose *value set* this tier does not
 * state. An `Erased` is whatever a caller put there and may be a folded `Maybe` of the same shape; a
 * `Parts` has no single host value at all, and `jsStoredRep` of one whose materialization is absent
 * is the representation itself; and `Void` has no value to be absent instead of.
 *
 * Everything else is a host value this tier builds and never builds as `null`. A `GcObject` is the
 * case worth naming, because `zeroOf` does answer `null` for a *recursive* one: that zero fills the
 * properties of an arm the tag does not name and is never a live payload, so it is not a counter
 * example - see `zeroOf`, where the cycle rule is argued.
 */
bool jsAdmitsAbsent(JsTarget& target, JsRepId id) {
    if(!id.valid()) return true;

    switch(target.rep(id).kind) {
        case JsRepKind::Nullable:
        case JsRepKind::Erased:
        case JsRepKind::Parts:
        case JsRepKind::Void:
            return true;
        default:
            return false;
    }
}

/*
 * Whether `null` is a value of this representation at all - which is a **different** question from
 * the one above, and the two come apart at exactly one kind.
 *
 * `jsAdmitsAbsent` asks whether a *live* value can be `null` and therefore whether a fold over it
 * would be one pattern standing for two. This asks whether a `null` literal at the representation is
 * well formed, and a host object's is: §3.3's "including an intentionally `null` environment or
 * header property" is one, and `zeroOf`'s cycle rule is the other - a recursive record has no finite
 * zero object, so the property of an arm the tag does not name holds `null`. Neither is ever a value
 * a reader of that property gets, which is why the fold is still legal over one.
 *
 * What it refuses is the shape `Analysis-IR-JS.md` §1.2 records as an old-backend defect: a `null`
 * written where a number, a bigint or a string was promised, which reads back as the wrong value
 * everywhere rather than throwing at the site that wrote it.
 */
bool jsNullIsAValue(JsTarget& target, JsRepId id) {
    if(!id.valid()) return true;

    switch(target.rep(id).kind) {
        case JsRepKind::Number:
        case JsRepKind::BigInt:
        case JsRepKind::HostString:
        case JsRepKind::HostBool:
        case JsRepKind::Void:
            return false;
        default:
            return true;
    }
}

/*
 * Whether `Maybe(Int)` - a fold whose payload is a **number** - is taken.
 *
 * On, and it is a measurement rather than a reading of the old JavaScript backend's (deleted with
 * `compiler/resolve` in J173). That one folded it (`Implementation-JS-Repr.md` §2: "a full-width
 * `Int` reaches every pattern of its word, so `Maybe(Int)` correctly falls through to `null`") and
 * the worry this names is V8's, not the encoding's: a slot holding a Smi *or* `null` is polymorphic
 * where a slot holding an object or `null` is not, so representation tracking can spend more on the
 * number than the removed allocation saves. Measured over `test/base` with this off and on, the two
 * `Base.js` timed interleaved best of nine: **2.058 s against 2.420 s** and 5,566,429 bytes against
 * 5,582,662. So it is not marginal and it is not the byte count that decides it - folding only the
 * object payloads is *slower than folding nothing*, which nothing predicted, and the reading that
 * fits is that a `Maybe(Int)` left as an object beside a folded `Maybe(Person)` makes the sites
 * that handle both polymorphic where one representation for every `Maybe` keeps them monomorphic.
 *
 * A constant rather than a flag, because a representation decision that varies between two builds of
 * the same program is two ABIs; if it ever has to be switchable it belongs on `JsTarget`.
 */
static constexpr bool kFoldsNumberPayloads = true;

/*
 * The niche fold - §3.2's `Nullable`, decided on the **logical type** and not on the name `Maybe`.
 *
 * Two constructors, one of them carrying nothing and the other carrying exactly one thing whose
 * representation cannot already be `null`. That is the whole rule, and it is a rule about shape
 * because the thing it is entitled to know is what the host can tell two values apart by: `null` is
 * the one pattern every host value that is not itself nullable leaves free, which is the same
 * sentence `Implementation-JS-Repr.md` §2 opens with. `Result(E, T)` has two payloads and no spare
 * pattern to tell them apart by, so it declines here exactly as `Niche::fits` declines it there.
 *
 * A **pinned** record declines: `@layout(js)` is the declaration saying what the host sees, and a
 * value that is sometimes `null` and sometimes an object is not the shape it was promised.
 *
 * Answers false and leaves `fold` untouched where the record does not fold.
 */
struct NicheFold {
    U16 payloadField = 0;
    I32 absentTag = 0;
    I32 presentTag = 0;
    DeclPtr<mir::Type> payload = nullptr;
    mir::Contract payloadContract;
};

static bool nicheFoldOf(JsTarget& target, DeclPtr<mir::Type> record,
                        DeclPtr<mir::OwnershipSchema> shape, NicheFold& fold) {
    auto& declared = *(mir::RecordType*)target.decl[record];
    if(declared.layout != mir::RecordLayout::Multi) return false;
    if(declared.pinned()) return false;
    if(declared.cons.size() != 2) return false;

    // Which of the two carries something, counting a unit field as nothing - a `Just({})` has a
    // field and no value, and the record is then an enum this arm never sees anyway.
    U16 present = 0xffff;
    U16 absent = 0xffff;
    U16 payloadField = 0;

    for(U16 i = 0; i < 2; i++) {
        auto arm = declared.cons.get(target.decl, i);

        U16 carried = 0;
        U16 which = 0;
        for(U16 f = 0; f < arm.count; f++) {
            auto index = U16(arm.first + f);
            auto field = declared.fields.get(target.decl, index);
            if(isUnitType(target, field.occurrence.type)) continue;

            carried++;
            which = index;
        }

        if(!carried) {
            if(absent != 0xffff) return false;
            absent = i;
        } else if(carried == 1) {
            if(present != 0xffff) return false;
            present = i;
            payloadField = which;
        } else {
            return false;
        }
    }

    if(present == 0xffff || absent == 0xffff) return false;

    auto field = declared.fields.get(target.decl, payloadField);
    if(field.host || field.boxed) return false;

    fold.payloadField = payloadField;
    fold.absentTag = I32(declared.cons.get(target.decl, absent).value);
    fold.presentTag = I32(declared.cons.get(target.decl, present).value);
    fold.payload = field.occurrence.type;
    fold.payloadContract = mir::fieldContract(target.decl, shape, field);
    return true;
}

/*
 * How many fields a record may have and still cross flat - a **band**, and both ends are measured.
 *
 * `Base.js` bytes over `test/base`, against 5,563,735 with this switched off entirely:
 *
 * ```text
 *   2..3   5,567,790   +4,055     3..4   5,546,598   -17,137    <- taken
 *   2..4   5,550,232  -13,503     3..8   5,551,604   -12,131
 *   2..5   5,555,810   -7,925     2..8   5,555,238   -8,497
 *   2..6   5,556,153   -7,582    2..16   5,555,238   -8,497
 * ```
 *
 * The **lower** bound is the surprise and is why this is a band rather than a ceiling: a two-field
 * record costs *more* flat than as an object. Two properties are one small literal and two
 * arguments are two positions at every call plus a materialization at every position that holds one
 * value, and below three fields the second outweighs the first. The ceiling is the ordinary one -
 * above four the argument lists grow faster than the literals they replace, which is the same
 * ceiling J72 found for the flat inlining budget and for the same reason.
 *
 * Node time at 3..4 is 0.9916x the same program without scalarization, interleaved best of nine,
 * which is inside the noise: what this buys is bytes.
 */
static constexpr U32 kScalarizeLeast = 3;
static constexpr U32 kScalarizeFields = 4;

/*
 * Whether a record crosses a boundary as its **fields** rather than as one object.
 *
 * Five conditions, and each of them is about something a caller could otherwise observe:
 *
 *  - **one constructor and no tag.** A sum is a run-time choice of which fields are live and the
 *    parts would be the whole union at every position;
 *  - **no `@layout` pin.** That is the declaration saying what the host sees, and a value that
 *    arrives as several arguments is not the object it was promised;
 *  - **not an environment record.** What a closure and the frame that built it share is the
 *    record's *identity* - `JsTarget::environmentRecords`, where the argument is written;
 *  - **every field a primitive this target duplicates by reading it** - a number, a bigint, a host
 *    string, a boolean. That is what makes a copy of the parts free and, more importantly, what
 *    makes it *correct*: a part holding an object would be shared by a duplicate that has no
 *    structure left to walk, which is `Analysis-IR-JS.md` §1.2's "a shared payload property later
 *    aliased an object copy" arriving from the other side;
 *  - **nothing to release and nothing to run at a copy**, which is one condition written as two
 *    tests. `Core.MapString`'s hundred leaked values and `test/resolve/ScalarRelease.yana` are what
 *    it costs to leave out, and no program distinguishes the two tests from each other: a record
 *    that owes a teardown is not trivially copyable either;
 *  - **`kScalarizeLeast` to `kScalarizeFields` of them**, both measured.
 *
 * The fourth is the one with a wrong answer behind it rather than a leak:
 * `test/resolve/ErasedCopy.yana` answers 25 where the unoptimized build answers -7 when a part may
 * hold an object.
 *
 * Deliberately **not** conditioned on the occurrence: `jsStoredRep` is what keeps the object at
 * every position holding one host value, so a stored record is unchanged whatever this answers -
 * which is the same layering J71 took for the window, and the half that took a measurement to see.
 */
static bool scalarizesHere(JsTarget& target, DeclPtr<mir::Type> type, mir::Type* declared,
                           JsShapeId shapeId) {
    auto& record = *(mir::RecordType*)declared;
    if(record.layout != mir::RecordLayout::Single || record.cons.size() != 1) return false;
    if(record.pinned()) return false;
    if(target.environmentRecords.contains(U32(type.offset))) return false;

    for(U16 i = 0; i < record.fields.size(); i++) {
        auto field = record.fields.get(target.decl, i);
        if(field.host || field.boxed) return false;
    }

    /*
     * Nothing to release and nothing to run at a copy.
     *
     * A teardown walks *storage* and parts are not storage - `emitTeardown` asks `jsManifest`, gets
     * a representation that is not an object, finds no transparent content under it and emits
     * nothing at all. `Core.MapString`'s hundred held values are what that looks like from the
     * outside, and it looks like a leak rather than like a refusal, which is why the condition is
     * here and not a case in the teardown walk. A record whose *field* is a transparent record with
     * an authored `Drop` is the shape that reaches it: the field's representation is a number and
     * every test above it passes.
     */
    if(jsOwesTeardown(target, type)) return false;
    if(!mir::isTriviallyCopyable(target.decl, type)) return false;

    auto& shape = target.shape(shapeId);

    // A record whose narrow fields share a `$pN` word is left an object. Its parts would be the
    // properties, and a projection of the field would then be a decode of a part - a second arm of
    // `FieldAddress` for a record `packFields` has already made cheaper to hold.
    if(shape.packedFields.size()) return false;

    if(shape.properties.size() < kScalarizeLeast || shape.properties.size() > kScalarizeFields) return false;

    for(auto& property: shape.properties) {
        if(!target.rep(property.rep).isPrimitive()) return false;
    }

    return true;
}

// ------------------------------------------------------------------------------------ packing

namespace {

// One property of a shape as the packing walk sees it - `packBits`' `valueBits`/`unitBits`, plus
// what the field is held as. Zero bits for a property that owns its storage.
struct JsPackMember {
    U32 valueBits = 0;
    U32 unitBits = 0;
    bool isSigned = false;
};

using JsPackMembers = SmallArray<JsPackMember, 8>;

} // namespace

/*
 * Whether a field held at this representation may share a word, and how many bits it owns.
 *
 * The native rule read off the `JsRep` rather than off the native `Repr`: **narrow** - fewer bits
 * than its own natural storage, so a `@bits(20) U32`, a `Bool`, an enum's discriminant and a
 * scalarized record are candidates and a `U8` or an `I32` is not - and a *number*. The second half
 * is where the two targets part: a `bigint` field (`Long`, `@bits(54)` to `@bits(63)`) is narrow
 * natively and is a different host type here, and there is no arithmetic that puts one into a
 * `number` exactly. The explicit `kMaxNumberBits` test says the same thing twice on purpose, since a
 * `Number` wider than that would be a representation bug that packing would turn into wrong bits.
 */
static bool packableRep(JsTarget& target, JsRepId id, U32& bits, bool& isSigned) {
    auto& rep = target.rep(id);
    if(rep.kind != JsRepKind::Number) return false;

    switch(rep.domain) {
        case NumberDomain::Bool01:   bits = 1; isSigned = false; break;
        case NumberDomain::Signed:   bits = rep.bits; isSigned = true; break;
        case NumberDomain::Unsigned: bits = rep.bits; isSigned = false; break;
        case NumberDomain::Float32:
        case NumberDomain::Float64:  return false;
    }

    if(!bits || bits > kMaxNumberBits) return false;
    return bits < mir::storageBitsFor(U8(bits));
}

/*
 * One member per property of `shape`, and whether the record may pack at all.
 *
 * The record-level refusals are the ones a caller could observe: a **pin** (`@layout(js)` promises
 * one property per field and `@layout(c)`/`@layout(packed)` are a wire format - none of them is
 * this compiler's to rearrange), a lambda's **environment** (its identity is what a closure shares),
 * and more than one constructor, since a sum's arms overlap and co-packing across them is a later
 * question. A field that is `@host` or `@box`, or whose value has an authored copy or release, never
 * packs - the second for `packableValue`'s native reason: those calls take the field's address. Nor
 * does a field a property witness names, which needs a property of its own for a body compiled once
 * to read by name (`JsTarget::witnessedFields`); so a record with one is never one number.
 */
static bool packMembers(JsTarget& target, DeclPtr<mir::Type> type, mir::RecordType& record,
                        const JsShape& shape, JsPackMembers& members) {
    if(record.layout != mir::RecordLayout::Single || record.cons.size() != 1) return false;
    if(record.pinned()) return false;
    if(target.environmentRecords.contains(U32(type.offset))) return false;

    for(auto& property: shape.properties) {
        JsPackMember member;

        if(property.role == JsPropertyRole::Field && property.field != JsProperty::kNoField) {
            auto field = record.fields.get(target.decl, property.field);
            auto held = field.occurrence.type;

            auto witnessed = target.witnessedFields.contains((U64(type.offset) << 32) |
                                                             U64(property.field));

            U32 bits = 0;
            bool isSigned = false;
            if(!field.host && !field.boxed && !witnessed &&
               packableRep(target, property.rep, bits, isSigned) &&
               mir::isTriviallyCopyable(target.decl, held) && !jsOwesTeardown(target, held)) {
                member.valueBits = bits;
                member.unitBits = mir::storageBitsFor(U8(bits));
                member.isSigned = isSigned;
            }
        }

        members.push(member);
    }

    return true;
}

static U32 addPacking(JsTarget& target, DeclPtr<mir::Type> type, const JsShape& shape,
                      const JsPackMembers& members, const ssa::PackOrder& order, Size from,
                      const ssa::PackedRun& run, const ssa::PackOffsets& offsets) {
    JsPacking packing;
    packing.span = U8(run.span);
    packing.word = jsNumberRep(target, NumberDomain::Unsigned, U8(run.span));
    packing.source = type;

    // In record order, which is the order a `Pack`'s operands are supplied in - placement order is
    // `packOrder`'s business and nobody downstream should have to know it.
    for(U32 slot = 0; slot < shape.properties.size(); slot++) {
        for(U16 i = 0; i < run.count; i++) {
            if(order[from + i] != slot) continue;

            auto& property = shape.properties[slot];
            packing.fields.push(JsPackedField {
                property.field, U8(offsets[i]), U8(members[slot].valueBits), members[slot].isSigned,
                property.rep
            });
        }
    }

    auto id = U32(target.packings.size());
    target.packings.push(::move(packing));
    return id;
}

/*
 * The whole record as one word, or `kNoPacking` - `JsTarget::scalarizeRecords`.
 *
 * Every field narrow and placed in one run of at most 53 bits. At least two fields, since a record
 * of one is transparent here before this is asked.
 *
 * A run that exactly *fills* its natural storage - 8, 16 or 32 bits - is packed too, since J86, and
 * that is where this departs from the native `scalarizeRecord`. The native rule declines it because
 * such a record is already a byte or a word there: it has no bits for a neighbour and no niche, so
 * packing would buy one decode per field and nothing else. What packing buys on this target is not
 * bits but **the object** - `PackTop`'s 8-bit `B1 {lo, hi}` was `{ $p0: n }`, one allocation per
 * value to hold one number. A niche is not a reason either: `Maybe(T)` here is `T | null` whatever
 * `T`'s representation is (J74).
 */
static U32 wholePackingOver(JsTarget& target, DeclPtr<mir::Type> type, mir::RecordType& record,
                            const JsShape& shape, bool add) {
    if(shape.properties.size() < 2) return JsProperty::kNoPacking;

    JsPackMembers members;
    if(!packMembers(target, type, record, shape, members)) return JsProperty::kNoPacking;

    ssa::PackOrder order;
    for(U16 i = 0; i < members.size(); i++) {
        if(!members[i].valueBits) return JsProperty::kNoPacking;
        order.push(i);
    }

    ssa::packOrder(members, order, false);

    ssa::PackOffsets offsets;
    auto run = ssa::packBits(members, order, 0, kMaxNumberBits, &offsets);
    if(run.count != order.size()) return JsProperty::kNoPacking;

    if(!add) return 0;
    return addPacking(target, type, shape, members, order, 0, run, offsets);
}

static bool samePacking(const JsPacking& a, const JsPacking& b) {
    if(a.word != b.word || a.fields.size() != b.fields.size()) return false;

    for(U32 i = 0; i < a.fields.size(); i++) {
        auto& x = a.fields[i];
        auto& y = b.fields[i];
        if(x.field != y.field || x.offset != y.offset || x.width != y.width ||
           x.isSigned != y.isSigned || x.rep != y.rep) {
            return false;
        }
    }

    return true;
}

/*
 * Whether this occurrence of a record is its packed word - asked by `jsRep` of every record.
 *
 * The row is the **type's**, computed from the shapeless shape and memoized on the handle, because
 * the place that reads it - a `FieldAddress` - knows the record and not the occurrence. An ownership
 * shape can only change a field into a reference, which is never narrow, so the two answers differ
 * only by one of them declining; where they do differ this declines, rather than packing a record
 * one way and projecting it the other.
 *
 * And that difference is asserted never to happen, because `jsWholePackingRow` - what a projection
 * asks - is keyed on the type alone and would still answer the row for an occurrence declined
 * here. Keying the row on (type, shape) instead was the alternative; it would be the fix the day a
 * shape can make a field narrow, and until then it is a second key nothing reaches.
 */
static const JsPacking* wholePackingOf(JsTarget& target, DeclPtr<mir::Type> type,
                                       DeclPtr<mir::OwnershipSchema> shape_, mir::RecordType& record,
                                       JsShapeId shapeId) {
    U32 row = JsProperty::kNoPacking;
    if(auto found = target.packingOfRecord.get(U32(type.offset))) {
        row = found.unwrap();
    } else {
        auto plain = shape_ ? jsShapeOf(target, type, nullptr, kNullLocation) : shapeId;
        row = wholePackingOver(target, type, record, target.shape(plain), true);
        target.packingOfRecord.add(U32(type.offset), row);
    }

    if(row == JsProperty::kNoPacking) return nullptr;
    if(!shape_) return &target.packings[row];

    auto here = wholePackingOver(target, type, record, target.shape(shapeId), true);
    assertTrue(here != JsProperty::kNoPacking);
    if(here == JsProperty::kNoPacking) return nullptr;

    // The row was only for comparing; it names nothing, so it is taken back off the table.
    auto same = samePacking(target.packings[here], target.packings[row]);
    target.packings.pop();
    assertTrue(same);
    return same ? &target.packings[row] : nullptr;
}

U32 jsWholePackingRow(JsTarget& target, DeclPtr<mir::Type> record) {
    if(!record) return JsProperty::kNoPacking;
    auto found = target.packingOfRecord.get(U32(record.offset));
    return found ? found.unwrap() : JsProperty::kNoPacking;
}

const JsPacking* jsWholePackingOf(JsTarget& target, DeclPtr<mir::Type> record) {
    auto row = jsWholePackingRow(target, record);
    return row == JsProperty::kNoPacking ? nullptr : &target.packings[row];
}

/*
 * The narrow fields of a record that stays an object, co-packed into `$pN` properties -
 * `JsTarget::packFields`.
 *
 * The native `placeArm`'s loop: candidates in `packOrder`, runs of at most 53 bits, and **a field
 * alone in a word is not packed** - it keeps its own property, and with it an address a borrow can
 * name without a shift. Candidates need a second candidate in the record for the same reason, which
 * is `packCandidate`'s. The `$pN` properties go after the plain ones, in run order, and the fields
 * they absorbed leave the property list: a construction supplies each word as one `Pack`.
 */
static void coPack(JsTarget& target, DeclPtr<mir::Type> type, mir::RecordType& record,
                   JsShape& shape) {
    JsPackMembers members;
    if(!packMembers(target, type, record, shape, members)) return;

    ssa::PackOrder candidates;
    for(U16 i = 0; i < members.size(); i++) {
        if(members[i].valueBits) candidates.push(i);
    }

    if(candidates.size() < 2) return;
    ssa::packOrder(members, candidates, false);

    SmallArray<bool, 16> absorbed;
    for(U32 i = 0; i < shape.properties.size(); i++) absorbed.push(false);

    struct Unit { U32 packing; };
    SmallArray<Unit, 4> units;

    Size at = 0;
    while(at < candidates.size()) {
        ssa::PackOffsets offsets;
        auto run = ssa::packBits(members, candidates, at, kMaxNumberBits, &offsets);
        if(run.count < 2) {
            at++;
            continue;
        }

        units.push(Unit { addPacking(target, type, shape, members, candidates, at, run, offsets) });
        for(U16 i = 0; i < run.count; i++) absorbed[candidates[at + i]] = true;
        at += run.count;
    }

    if(!units.size()) return;

    Array<JsProperty> kept;
    for(U32 i = 0; i < shape.properties.size(); i++) {
        if(!absorbed[i]) kept.push(shape.properties[i]);
    }

    for(U32 u = 0; u < units.size(); u++) {
        char buffer[16] = { '$', 'p' };
        auto length = 2 + show(U64(u), buffer + 2, sizeof(buffer) - 2);

        auto& row = target.packings[units[u].packing];
        JsProperty unit;
        unit.key = jsInternText(target.context, target.interned, StringView { buffer, length });
        unit.rep = row.word;
        unit.role = JsPropertyRole::StorageUnit;
        unit.packing = units[u].packing;

        auto slot = U16(kept.size());
        kept.push(unit);

        for(auto& bits: row.fields) {
            shape.packedFields.push(JsShapePackedField { bits.field, slot, bits });
        }
    }

    shape.properties = ::move(kept);
}

static bool isBoolRecord(JsTarget& target, DeclPtr<mir::Type> type) {
    return target.boolRecord && mir::recordDeclarationOf(target.decl, type) == target.boolRecord;
}

static bool isStringRecord(JsTarget& target, DeclPtr<mir::Type> type) {
    return target.stringRecord && mir::recordDeclarationOf(target.decl, type) == target.stringRecord;
}

JsShapeId jsShapeOf(JsTarget& target, DeclPtr<mir::Type> record,
                    DeclPtr<mir::OwnershipSchema> shape_, LocationId where) {
    /*
     * Keyed on the record **and the occurrence's ownership shape** - the same pair `membersOf` is
     * keyed on natively, and for the same reason said in JavaScript.
     *
     * `Maybe('v)` is `Maybe(v)` with a shape naming the payload, so a `findMut` answering
     * `Maybe(&Int)` and an `insert` answering `Maybe(Int)` are one record type and two different
     * things to hold: one property is the `{$o, $k}` descriptor that can be written through and the
     * other is the number. One shape for both made the write through the reference a whole-value
     * store into a number, which is the emitter refusing rather than a wrong answer - and the
     * refusal is what said this key was one field short.
     */
    auto key = (U64(record.offset) << 32) | U64(shape_.offset);
    if(auto found = target.shapeOfRecord.get(key)) return found.unwrap();

    auto& declared = *(mir::RecordType*)target.decl[record];

    /*
     * Interned by ID *before* its members are filled - §5.1.
     *
     * That is what closes a recursive record: a field naming the record back reaches `jsRep`, which
     * reaches here, which finds the id and answers it. The properties it sees at that moment are
     * empty, and by the time anything reads them the fill below has run - which holds because
     * nothing between here and the end of this function reads a shape's property list.
     */
    auto id = JsShapeId(target.shapes.size());
    target.shapes.push(JsShape {});
    target.shapeOfRecord.add(key, id);

    JsShape shape;
    shape.kind = JsShapeKind::Object;
    shape.source = record;

    /*
     * A sum's `$tag`, first and before every payload.
     *
     * In front for the reason the native layout puts a stored discriminant at offset zero: what
     * reads a tag is a `match` on a value it has proved nothing about, so the property has to be one
     * every constructor supplies. Construction order being the hidden class means "supplies" is
     * literal - every arm's object literal writes it in this position.
     */
    if(declared.layout == mir::RecordLayout::Multi) {
        auto range = mir::tagRangeOf(target.decl, declared);
        shape.properties.push(JsProperty {
            jsInternText(target.context, target.interned, key::tag),
            jsNumberRep(target, range.isSigned ? NumberDomain::Signed : NumberDomain::Unsigned,
                        range.bits ? range.bits : 8),
            JsPropertyRole::Tag, JsProperty::kNoField
        });
    }

    /*
     * Every field of every constructor, in record order.
     *
     * All of them rather than the live arm's, which is what keeps one sum to one hidden class: which
     * constructor a value holds is a run-time fact and the shape is not, so an object literal lists
     * the whole union and the other arms' properties hold their representation's zero. A copy then
     * copies all of them to the same zeros, and a `match` reads the one it proved.
     */
    for(U16 i = 0; i < declared.fields.size(); i++) {
        auto field = declared.fields.get(target.decl, i);
        if(isUnitType(target, field.occurrence.type)) continue;

        // The field's own contract, or the **occurrence's** where a qualified type argument said
        // the position is somebody else's - `mir::fieldContract`, which is the one place the two
        // sources are read in order, and the same call `lower/repr.cpp`'s `membersOf` makes.
        mir::Occurrence held { field.occurrence.type,
                               mir::fieldContract(target.decl, shape_, field), nullptr };

        shape.properties.push(JsProperty {
            propertyKey(target, declared, i), fieldRepOf(target, held, where),
            JsPropertyRole::Field, i
        });
    }

    /*
     * **The whole record first, and co-packing only for one that cannot be a number** - the old
     * JavaScript backend's order (`scalarizeTuple` before `placeArm`), and the one that matters: a
     * record co-packed first had one `$p0` property left and no longer met "at least two fields",
     * so under both switches a 37-bit `{kind, live, count, age}` became `{ $p0: n }` - an object
     * per element and a clone per update, where `scalarizeRecords` alone made it a number. The
     * question is asked here without adding a row (`wholePackingOver`'s dry run), because the row
     * itself is `jsRep`'s and is keyed on the type.
     */
    if(target.packFields &&
       !(target.scalarizeRecords &&
         wholePackingOver(target, record, declared, shape, false) != JsProperty::kNoPacking)) {
        coPack(target, record, declared, shape);
    }

    target.shapes[id.index] = ::move(shape);
    return id;
}

JsShapeId jsHostShapeOf(JsTarget& target, DeclPtr<mir::Type> record, LocationId where) {
    // Keyed apart from the record's own shape, which a transparent record does not have: the high
    // bit of the key is what says which of the two questions is being asked.
    auto key = U64(record.offset) | (U64(1) << 63);
    if(auto found = target.shapeOfRecord.get(key)) return found.unwrap();

    auto& declared = *(mir::RecordType*)target.decl[record];

    JsShape shape;
    shape.kind = JsShapeKind::Host;
    shape.source = record;

    for(U16 i = 0; i < declared.fields.size(); i++) {
        auto field = declared.fields.get(target.decl, i);
        if(!field.host || isUnitType(target, field.occurrence.type)) continue;

        shape.properties.push(JsProperty {
            propertyKey(target, declared, i), fieldRepOf(target, field.occurrence, where),
            JsPropertyRole::Field, i
        });
    }

    auto id = JsShapeId(target.shapes.size());
    target.shapes.push(::move(shape));
    target.shapeOfRecord.add(key, id);
    return id;
}

JsRepId jsRep(JsTarget& target, DeclPtr<mir::Type> type, LocationId where) {
    return jsRep(target, type, nullptr, where);
}

JsRepId jsRep(JsTarget& target, DeclPtr<mir::Type> type, DeclPtr<mir::OwnershipSchema> shape_,
              LocationId where) {
    if(!type) return jsVoidRep(target);

    // Two memos: the shapeless one is the overwhelming majority and stays keyed on the handle
    // alone, which is what keeps the common path one probe.
    if(!shape_) {
        if(auto found = target.repOfType.get(U32(type.offset))) return found.unwrap();
    } else {
        auto key = (U64(type.offset) << 32) | U64(shape_.offset);
        if(auto found = target.repOfShaped.get(key)) return found.unwrap();
    }

    auto computed = [&]() -> JsRepId {
        auto declared = target.decl[type];

        switch(declared->kind) {
            case mir::Type::Unit:
                return jsVoidRep(target);

            case mir::Type::Int: {
                auto& integer = *(mir::IntType*)declared;
                auto bits = jsIntBits(integer);

                // Above 53 the host's `number` stops counting by ones, so the value is a `bigint` -
                // Analysis-JS.md §2.1's tower, with the 33-to-53 band a `number` whose operators
                // come from `codegen/js/wide.cpp`.
                if(bits > kMaxNumberBits) return jsBigIntRep(target, integer.isSigned);

                return jsNumberRep(target,
                                   integer.isSigned ? NumberDomain::Signed : NumberDomain::Unsigned,
                                   bits);
            }

            case mir::Type::Float: {
                auto& real = *(mir::FloatType*)declared;
                return jsNumberRep(target,
                                   real.bits == 32 ? NumberDomain::Float32 : NumberDomain::Float64,
                                   U8(real.bits));
            }

            /*
             * A `%a` on this target **is** the host array - `lib/Native/Host.js.yana` opens by
             * saying so, and it is what lets an element place be typed with `elementTypeOf` and
             * nothing added. There is no address here for a pointer to be one of.
             */
            case mir::Type::Pointer: {
                auto to = ((mir::PointerType*)declared)->to;
                auto element = isUnitType(target, to) ? jsErasedRep(target)
                                                      : jsRep(target, to, where);
                return jsHostArrayRep(target, element);
            }

            case mir::Type::Array: {
                auto& array = *(mir::ArrayType*)declared;

                // The element is one host value - `jsStoredRep`, so a row of vectors is a row of
                // lane rows rather than a row of a representation nothing can be stored at.
                return jsHostArrayRep(target, jsStoredRep(target, jsRep(target, array.element, where)),
                                      array.hasFixedLength() ? U32(array.length) : 0);
            }

            // §3.2.1: one pair for both, and `Iter` gets no family of its own.
            case mir::Type::Fun:
            case mir::Type::Iter:
                return jsFunRep(target);

            case mir::Type::Record: {
                if(isStringRecord(target, type)) return jsStringRep(target);
                if(isBoolRecord(target, type)) return jsNumberRep(target, NumberDomain::Bool01, 1);

                auto& record = *(mir::RecordType*)declared;

                // An enum is its discriminant and nothing else, so it is a number here exactly as it
                // is a machine word natively - and so is a sum whose payloads all substituted away.
                if(record.layout == mir::RecordLayout::Enum) {
                    auto range = mir::tagRangeOf(target.decl, record);
                    return jsNumberRep(target,
                                       range.isSigned ? NumberDomain::Signed : NumberDomain::Unsigned,
                                       range.bits ? range.bits : 8);
                }

                if(auto content = transparentContentOf(target, type)) {
                    return jsRep(target, content, where);
                }

                /*
                 * The **niche fold** - `Maybe(Person)` is `Person | null` and `Maybe(Int)` is
                 * `number | null`, which is what removes the object a `Just` costs.
                 *
                 * Decided here, on the type, rather than at the sites that read a tag or build a
                 * value: everything below reads the *representation* and there is exactly one place
                 * where a record stops being an object. The payload's representation is asked for
                 * first and the fold declines where it can already be `null`, which is §3.2's
                 * nesting rule - so `Maybe(Maybe(T))` is a tagged object over a folded one.
                 *
                 * `jsRep` memoizes, and the payload ask below is a *recursion into this table*: a
                 * self-referential sum reaches `interning` and closes there, exactly as a record
                 * field naming its own record does.
                 */
                NicheFold fold;
                if(nicheFoldOf(target, type, shape_, fold)) {
                    mir::Occurrence held { fold.payload, fold.payloadContract, nullptr };
                    auto payload = fieldRepOf(target, held, where);

                    auto folds = !jsAdmitsAbsent(target, payload) &&
                                 (kFoldsNumberPayloads ||
                                  target.rep(payload).kind != JsRepKind::Number);

                    if(folds) {
                        auto range = mir::tagRangeOf(target.decl, record);

                        JsRep rep;
                        rep.kind = JsRepKind::Nullable;
                        rep.content = payload;
                        rep.tag = jsNumberRep(target,
                            range.isSigned ? NumberDomain::Signed : NumberDomain::Unsigned,
                            range.bits ? range.bits : 8);
                        rep.absentTag = fold.absentTag;
                        rep.presentTag = fold.presentTag;
                        rep.payloadField = fold.payloadField;
                        rep.source = type;
                        return internRep(target, rep);
                    }
                }

                JsRep rep;
                rep.kind = JsRepKind::GcObject;
                rep.shape = jsShapeOf(target, type, shape_, where);
                rep.source = type;

                /*
                 * A record of **narrow numbers** is one number - `JsTarget::scalarizeRecords`, and
                 * the old JavaScript backend's `scalarizeTuple`. Asked before the object is
                 * interned and before `scalarizesHere`, because a packed record is a primitive and
                 * every rule below is about something that has properties.
                 */
                if(target.scalarizeRecords) {
                    if(auto packing = wholePackingOf(target, type, shape_, record, rep.shape)) {
                        return packing->word;
                    }
                }

                auto object = internRep(target, rep);

                /*
                 * A **view record** is its fields, flat - §3.2's narrow-reference row read for the
                 * other multi-value thing this target carries everywhere.
                 *
                 * `ArrayView(a)` is `{items, length, offset}`, and one was built at *every* borrow
                 * of an array: 981 of them in `Base.js` against the old JavaScript backend's 137,
                 * which was the largest single line of J69 §6's size and time gap. As parts it is
                 * three values that cross a call, an edge and a local flat, a field read of one is
                 * the part and no object exists at all - which is the same arrangement the old
                 * backend reached by flattening `xs` into `xs$items, xs$length, xs$offset` at a
                 * direct call, taken one step further because a block argument is a position this
                 * tier flattens too.
                 *
                 * On the *record* rather than on the occurrence, which is the half that took a
                 * measurement to see: `slice` builds an owned `ArrayView` value and a `represent`
                 * relabels it as the window, so a rule keyed on `AccessForm::View` alone would have
                 * built the object at the aggregate and split it one instruction later.
                 * `jsStoredRep` is the object again, so a *stored* window - a field, a local the
                 * demand pass kept, an erased argument - is unchanged.
                 */
                if(mir::viewRowOfRecord(target.decl, target.program, type)) {
                    auto& built = target.shape(rep.shape);

                    SmallArray<JsRepId, 4> parts;
                    for(auto& property: built.properties) parts.push(property.rep);
                    if(parts.size()) return jsPartsRep(target, held(parts), object);
                }

                /*
                 * A small record of **scalars**, flat - the window's arrangement asked of an
                 * ordinary `data`, which is J72 §8's sketch and the profile's second finding.
                 *
                 * `DateStyle` is the shape: eight numbers that crossed every call as one object and
                 * were rebuilt at every preset, where the old JavaScript backend passed eight
                 * arguments. `jsStoredRep` is the object again at every position that holds one
                 * host value, so a field, a local the demand pass kept and an erased argument are
                 * unchanged, and what becomes flat is exactly the positions this tier flattens: an
                 * argument, a result, an edge and the aggregate that would otherwise allocate.
                 */
                if(scalarizesHere(target, type, declared, rep.shape)) {
                    auto& built = target.shape(rep.shape);

                    SmallArray<JsRepId, 8> parts;
                    for(auto& property: built.properties) parts.push(property.rep);
                    return jsPartsRep(target, held(parts), object, false, true);
                }

                return object;
            }

            /*
             * A value whose representation the caller decides - a type variable in a body compiled
             * once.
             *
             * `Erased` rather than a report, because a *type* being erased is not by itself
             * something this tier cannot do: a value of one is passed through and never taken apart
             * here. What reports is the site that has to look inside one - a projection, a
             * construction, a metric - and it reports there, where there is a location to point at.
             */
            case mir::Type::Variable:
            case mir::Type::Const:
                return jsErasedRep(target);

            /*
             * A vector is `lanes` values and a mask is `lanes` host booleans - §3.2's `Parts` rows,
             * and Implementation-Vector.md §7, which makes this the representation at *any* lane
             * count rather than an optimization above a boxed one.
             *
             * What that buys is that no operation over a vector builds anything: an add is `lanes`
             * additions over `lanes` variables, a splat is one value named `lanes` times, and a lane
             * read, a lane write and a shuffle emit nothing at all. The materialized form is a
             * **plain** host row, and it exists only for the positions one value is the only thing
             * that fits: a field, an element, a `return`, an erased argument.
             *
             * Plain rather than typed since J136, which was the old JavaScript backend's choice
             * too. A typed row bought nothing here: every lane is already coerced to its width by
             * the operation that made it (`|0`, `>>> 0`, `Math.fround`), a host transfer reads and
             * writes the *parts* rather than the row, and no bitcast reaches a row's buffer - a
             * lane-width change is refused. What it cost was V8's escape analysis, which elides a
             * `[a, b, c, d]` that a caller splits straight back into lanes and does not elide a
             * `new Int32Array([...])`: a `@noinline` `Vec(I32, 4)` round trip measured 8.5x the
             * old backend's CPU for that reason alone (J94 §6.3 H).
             *
             * A **mask's** lanes are host booleans rather than the lane-width all-ones the native
             * Repr describes, in the same way and for the same reason LLVM's `<N x i1>` is - see
             * `JsRepKind::HostBool`. The materialized row of one is a plain array, since a boolean
             * has no typed row.
             */
            case mir::Type::Vector: {
                auto& vector = *(mir::VectorType*)declared;
                auto lanes = mir::vectorLanes(target.decl, type);

                if(!lanes || lanes > 64) {
                    target.notYet("a vector whose lane count this target cannot see"_v, type, where);
                    return jsErasedRep(target);
                }

                auto lane = vector.isMask ? jsHostBoolRep(target)
                                          : jsRep(target, vector.lane, where);

                /*
                 * The one lane this target has no form for, and it is the host's answer rather than
                 * a gap here: a `number` holds a 40-bit integer exactly but the operators stop at
                 * 32, so `wide.cpp`'s helpers are what arithmetic on one costs - `lanes` calls per
                 * operation, against one machine instruction. `Vec(WideInt)` is refused rather than
                 * emitted, on the same terms the old JavaScript backend refused it.
                 */
                auto& laneRep = target.rep(lane);
                if(laneRep.isWideNumber()) {
                    target.notYet("a vector of a 33-to-53-bit lane, which has no JavaScript form"_v,
                                  type, where);
                    return jsErasedRep(target);
                }

                SmallArray<JsRepId, 16> parts;
                for(U32 i = 0; i < lanes; i++) parts.push(lane);

                return jsPartsRep(target, held(parts),
                                  jsHostArrayRep(target, lane, lanes, false), true);
            }

            case mir::Type::Atomic:
                target.notYet("an atomic"_v, type, where);
                return jsErasedRep(target);

            case mir::Type::KindCount:
                break;
        }

        target.internalError("met a type kind it has no arm for"_v, where);
        return jsErasedRep(target);
    }();

    if(!shape_) target.repOfType.add(U32(type.offset), computed);
    else target.repOfShaped.add((U64(type.offset) << 32) | U64(shape_.offset), computed);

    return computed;
}

JsTeardownStep jsTeardownStep(JsTarget& target, DeclPtr<mir::Type> type, bool runDrop,
                              bool runReclaim) {
    JsTeardownStep step;
    if(!type) return step;

    auto decl = target.decl;
    auto ownership = mir::ownershipOf(decl, type);

    step.authoredDrop = runDrop && ownership.drop == mir::TeardownKind::Authored;
    step.authoredReclaim = runReclaim && ownership.reclaim == mir::TeardownKind::Authored;

    // An authored instance replaces the derived walk *for its own half*, which is why the two flags
    // travel down rather than one - except an authored `Drop` over a derived `Reclaim`, whose body
    // finishes the members itself (`mir::Ownership::dropOwnsValue`).
    step.childDrop = runDrop && ownership.drop == mir::TeardownKind::Derived;
    step.childReclaim = runReclaim && ownership.reclaim == mir::TeardownKind::Derived &&
                        !(runDrop && ownership.dropOwnsValue());
    if(!step.childDrop && !step.childReclaim) return step;

    auto declared = decl[type];

    switch(declared->kind) {
        case mir::Type::Fun:
        case mir::Type::Iter:
            // Nothing at all where no lambda in this program owes an authored teardown, which is
            // §3.9 rather than an optimization - see `emitTeardown`, where the two halves the
            // native release has and this one does not are argued.
            if(target.closureOwesTeardown) step.kind = JsTeardownKind::FunValue;
            return step;

        case mir::Type::Array: {
            auto& array = *(mir::ArrayType*)declared;
            if(!array.hasFixedLength()) return step;

            step.kind = JsTeardownKind::FixedArray;
            step.content = array.element;
            return step;
        }

        case mir::Type::Variable:
            step.kind = JsTeardownKind::Descriptor;
            return step;

        case mir::Type::Record:
            step.kind = ((mir::RecordType*)declared)->layout != mir::RecordLayout::Single
                ? JsTeardownKind::Sum : JsTeardownKind::Record;
            return step;

        default:
            step.kind = JsTeardownKind::Unsupported;
            return step;
    }
}

bool jsOwesTeardown(JsTarget& target, DeclPtr<mir::Type> type, U32 depth) {
    if(!type || depth > 16) return false;

    auto decl = target.decl;
    auto step = jsTeardownStep(target, type, true, true);
    if(step.authoredDrop || step.authoredReclaim) return true;

    switch(step.kind) {
        /*
         * A type this target has no walk for: **no**, which is the one arm that is not the
         * emitter's answer and is deliberate. The emitter *reports* there, so a program that
         * reaches it does not compile at all, and answering yes here would turn a shape nothing in
         * the corpus builds into a `TypeDrop` written for the sole purpose of reporting from inside
         * a body rather than at the site.
         */
        case JsTeardownKind::Nothing:
        case JsTeardownKind::Unsupported:
            return false;

        // The pair, released through the shared helper - the step already asked whether any lambda
        // in this program owes anything, so reaching this arm is the answer.
        case JsTeardownKind::FunValue:
            return true;

        /*
         * A value of a type a body compiled once cannot see: **yes**, for the reason the
         * descriptor's teardown cell is never null. What runs is a run-time fact, so the honest
         * static answer is that something may. Answering no is what made a sum whose payload is a
         * type variable skip its own `TypeDrop`, since the sum arm asks this predicate first.
         */
        case JsTeardownKind::Descriptor:
            return true;

        case JsTeardownKind::FixedArray:
            return jsOwesTeardown(target, step.content, depth + 1);

        /*
         * A sum and a single-constructor record are one question here and two spellings there: what
         * is owed is whatever any member owes, and *which* members there are is the whole of the
         * difference. A transparent record's own field is among these too, so this answers for the
         * `transparentContentOf` route `emitTeardown` takes as well.
         */
        case JsTeardownKind::Sum:
        case JsTeardownKind::Record: {
            auto& record = *(mir::RecordType*)decl[type];
            for(U16 i = 0; i < record.fields.size(); i++) {
                auto field = record.fields.get(decl, i);

                // `tearsDownField` and not the type's own answer: a field in an origin slot is
                // storage somebody else owns, which is `emitTeardown`'s rule and this one's.
                if(!mir::tearsDownField(decl, field)) continue;
                if(jsOwesTeardown(target, field.occurrence.type, depth + 1)) return true;
            }

            return false;
        }
    }

    return false;
}

JsManifestation jsManifest(JsTarget& target, const mir::Occurrence& occurrence, LocationId where) {
    JsManifestation manifest;

    /*
     * A **window** manifests as the view record the library paired with the owner - `viewRecordOf`,
     * and the same table `manifestNative` reads.
     *
     * §3.2.2 decides the other way for `String` and that decision still stands here: it is the
     * `stringView` row being *unclaimed* on this target that makes it so, since `viewRowOf` then
     * answers nothing and a shared string is the host string. Where the library does declare both
     * halves - `Array(a)` and `ArrayView(a)` in `Array.js.yana` - the window is that record, because
     * its own bodies read `self.offset` and a manifestation that answered the owner would be a
     * projection into a record the value does not have.
     *
     * Since J71 that record is a **`Parts`** representation rather than a `GcObject` - see the view
     * row in `jsRep` - so a window costs nothing at a local, an edge or a direct call and becomes an
     * object only where one value is the only thing that fits.
     */
    auto held = occurrence.type;
    auto window = false;
    if(occurrence.contract.form() == mir::AccessForm::View) {
        if(auto view = mir::viewRecordOf(target.context, target.program, occurrence)) {
            held = view;
            window = true;
        }
    }

    /*
     * The shape only where it **takes a member away** - `mir::qualifiesInterior`, which is the one
     * test that tells a qualification from a statement about provenance.
     *
     * `Reader(a')` says which of a signature's origin sets fills a position `Reader` already has:
     * same members, same properties, same hidden class. Keying the representation on the handle
     * anyway made two ids for one object, and a value built under one and read under the other is a
     * mismatch the verifier reports at every call between them - 80 of them across `lib/Format`
     * alone. `Maybe('v)` is the case that genuinely differs, and it is what this admits.
     */
    auto shape = held == occurrence.type && mir::qualifiesInterior(target.decl, occurrence)
        ? occurrence.shape : DeclPtr<mir::OwnershipSchema>(nullptr);

    manifest.rep = jsRep(target, held, shape, where);

    auto& rep = target.reps[manifest.rep.index];
    if(rep.kind == JsRepKind::Erased) manifest.erased = true;

    auto contract = occurrence.contract;
    if(contract.isOwned()) return manifest;

    /*
     * A **window** is a value whatever the access says, and that is not this target's economy.
     *
     * `manifestNative` puts its view row *above* the reference-value row and hands the record back
     * at `mir::owned(view)`: what `&xs: [Int]` may write through is the elements, and the descriptor
     * itself is never rebound - `sliceMut` returns a fresh one. Reaching the `needsCell` test below
     * would have wrapped the three parts in a cell naming storage nobody writes.
     */
    if(window) return manifest;

    /*
     * A loan, and the one question this layer answers about one: does it need somewhere to rebind?
     *
     * A **shared** loan never does - nothing writes through it - and a shared window manifests as
     * the host value itself rather than as three values carried everywhere. §3.2.2 argues that for
     * `String` and the argument is not `String`'s: a copy is *weaker* than a window, so the only
     * thing a program could observe is a write through the owner while a window into it is live, and
     * that is precisely what the loan checker refuses. The two targets accept the same programs and
     * answer the same values.
     *
     * A **unique** loan of a host object does not either: writing through a `&Point` writes
     * properties of an object that stays the same object, so the reference is the object. One of
     * anything else does, because the write has to replace a binding somebody else can see - a host
     * string is immutable, a number is a value, and a niche-folded payload has to be able to become
     * absent.
     */
    /*
     * A **unique** loan of a host object does not need one either: writing through a `&Point` writes
     * properties of an object that stays the same object, so the reference is the object.
     *
     * A host *array* is not one of those, and the distinction is the reason this tests `GcObject`
     * rather than `isObjectLike`. A transparent `Array(Handle)` is the bare row, so `self.items = x`
     * through a `&` of one is a rebinding of the whole value rather than a write into it - there is
     * no property to assign, because the record has no object of its own. `hostGrow` is where that
     * arrives, and it arrives in `lib/` rather than in a fixture.
     */
    /*
     * A **unique loan of a scalarized record is the object**, which is the one occurrence that
     * undoes the flattening above and does so for the same sentence: writing through a `&Point`
     * writes properties of an object that stays the same object, and there is nothing for a write
     * to land in when the value is N arguments. So a `&` position takes the materialized form and
     * is its own reference, exactly as an unscalarized record's is.
     *
     * A **vector** is not one of these - its parts are lanes and `&Vec` is the box it always was -
     * which is what `lanewise` is asked here for.
     */
    if(contract.access() == mir::Access::Unique && rep.kind == JsRepKind::Parts && !rep.lanewise) {
        auto stored = jsStoredRep(target, manifest.rep);
        if(stored.valid() && target.reps[stored.index].kind == JsRepKind::GcObject) {
            manifest.rep = stored;
            return manifest;
        }
    }

    if(contract.access() == mir::Access::Unique && rep.kind != JsRepKind::GcObject &&
       rep.kind != JsRepKind::Void) {
        manifest.needsCell = true;
    }

    return manifest;
}

} // namespace jsir
