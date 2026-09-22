#include "build.h"
#include "../../resolve_ssa/js/name.h"

/*
 * Identifiers.
 *
 * Every name the emitted file contains is decided here and decided once, when the Name is created
 * rather than when it is printed. `+=(Int)` and `Num(Int).+` are real function names in this
 * compiler, so sanitizing is the common case rather than a corner one, and two names that sanitize
 * alike have to still end up as two identifiers - which is a solvable problem exactly because it is
 * answered in one place instead of at every use site.
 */

namespace js {

/*
 * Whether `text` hashes to an id the context already holds for some *other* text.
 *
 * A StringId is the text's 32-bit hash and nothing else (`Context::addIdentifier`), and registering a
 * second text under one replaces the first's - so every name that id stood for, a string literal
 * included, is printed as the newcomer from then on. This backend makes a name for every lane of
 * every vector, which is enough of them for a collision to be a matter of time: the lane `v67$10`
 * of a byte lookup and the literal `"′′′"` in NormalizationTables are one id, and a program that
 * held both answered U+2034's case fold as the text `v67$10`.
 */
using jsir::jsReservedWord;
using jsir::jsSanitize;

static bool collides(JsShared& g, StringView text) {
    auto& known = g.context.find(Context::nameHash(text));
    return known.textLength && !(StringView { known.text, known.textLength } == text);
}

/*
 * Interning a name the generator built rather than one the source contained.
 *
 * Context::addUnqualifiedName keeps the pointer it is given instead of copying: every other caller
 * hands it a slice of source text, which outlives the compilation. Nothing here does, so the text
 * is copied into the string arena first - and only the first time, since a field name is built
 * again at every projection that reads it.
 */
StringId internText(JsShared& g, StringView text) {
    return jsir::jsInternText(g.context, g.interned, text);
}

Name uniqueName(JsShared& g, StringView text, bool local) {
    char buffer[512];
    auto length = jsSanitize(text, buffer, sizeof(buffer) - 16);

    if(jsReservedWord(StringView { buffer, length })) buffer[length++] = '$';

    // A name already handed out gets a numeric tail. The counter is kept per base name rather than
    // globally, so an identifier that never collides never grows one.
    // One whose hash already belongs to other text counts as handed out too - see `collides`.
    auto taken = [&](StringView text) {
        if(collides(g, text)) return true;

        auto id = Context::nameHash(text);
        return g.moduleNames.contains(id) || (local && g.localNames.contains(id));
    };

    if(taken(StringView { buffer, length })) {
        auto base = length;
        for(U32 counter = 1;; counter++) {
            length = base;
            buffer[length++] = '$';
            length += show(U64(counter), buffer + length, sizeof(buffer) - length);

            if(!taken(StringView { buffer, length })) break;
        }
    }

    auto candidate = internText(g, StringView { buffer, length });

    if(local) {
        g.localNames.add(candidate);
    } else {
        g.moduleNames.add(candidate);
    }

    return Name { candidate };
}

Name generatedName(JsShared& g, StringView prefix, U32 index) {
    char buffer[64];
    copy(prefix.ptr, buffer, prefix.length);

    auto length = prefix.length;
    length += show(U64(index), buffer + length, sizeof(buffer) - length);

    return uniqueName(g, StringView { buffer, length }, true);
}

Name valueName(JsShared& g, Value& value) {
    if(value.name) {
        auto text = g.context.findName(value.name);
        return uniqueName(g, stringView(text), true);
    }

    return generatedName(g, "v"_v, value.id);
}

/*
 * One part of a value this target carries in pieces, named after the value itself.
 *
 * `flip(p$o, p$k, p$s)` rather than three anonymous slots, and `apply(f$c, f$e, x)` rather than two,
 * so that emitted source still reads as one value taken apart rather than as several unrelated
 * parameters. Each part goes through uniqueName like any other local, since the base name it is
 * built from is already taken.
 *
 * Shared by the two multi-part representations - the reference triple and the function-value pair -
 * because naming a part is the same job in both and the suffix is the whole of the difference.
 */
Name partName(JsShared& g, Value& value, StringView suffix) {
    char buffer[512];
    Size length = 0;

    if(value.name) {
        auto text = stringView(g.context.findName(value.name));
        length = text.length < sizeof(buffer) - 16 ? text.length : sizeof(buffer) - 16;
        copy(text.ptr, buffer, length);
    } else {
        buffer[length++] = 'v';
        length += show(U64(value.id), buffer + length, sizeof(buffer) - length);
    }

    copy(suffix.ptr, buffer + length, suffix.length);
    return uniqueName(g, StringView { buffer, length + suffix.length }, true);
}

/*
 * One lane of a vector - `x$0`, `x$1`, ... - which is the third of the multi-part representations
 * and the only one whose part count is a property of the type rather than a constant.
 *
 * Beside `partName` because it is the same job with a computed suffix, and here rather than in
 * inst.cpp because a vector phi's lanes are declared by gen.cpp and assigned by flow.cpp.
 */
Name laneName(JsShared& g, Value& value, U32 lane) {
    char suffix[16];
    Size length = 0;

    suffix[length++] = '$';
    length += show(U64(lane), suffix + length, sizeof(suffix) - length);

    return partName(g, value, StringView { suffix, length });
}

Name propertyName(JsShared& g, StringView text) {
    char buffer[512];
    auto length = jsSanitize(text, buffer, sizeof(buffer));
    return Name { internText(g, StringView { buffer, length }) };
}

Name literalName(JsShared& g, StringView text) {
    return Name { internText(g, text) };
}

/*
 * The property a run of co-packed fields shares, named after where in the record it sits.
 *
 * No field's own name would be right for it: `$p0` holds `version`, `flags` and `length` at once, and
 * reading any of them is a shift and a mask of the one number. The offset is what makes it unique
 * within a record - two packed words are two words at two offsets - and stable, since it is the same
 * number the layout dump prints.
 */
Name packedWordName(JsShared& g, U32 offset) {
    char buffer[32];
    buffer[0] = '$';
    buffer[1] = 'p';

    auto length = 2 + show(U64(offset), buffer + 2, sizeof(buffer) - 2);
    return Name { internText(g, StringView { buffer, length }) };
}

Name fieldName(JsShared& g, StringId name, U16 index) {
    if(name) {
        auto text = g.context.findName(name);
        return propertyName(g, stringView(text));
    }

    char buffer[32];
    buffer[0] = 'f';
    auto length = 1 + show(U64(index), buffer + 1, sizeof(buffer) - 1);
    return Name { internText(g, StringView { buffer, length }) };
}

/*
 * One word of a function-value field, named after the field - `run$c` and `run$e`.
 *
 * A property rather than an identifier, so it needs no disambiguation: two fields cannot share a
 * name, so neither can their words, and the suffixes are ones `propertyName` never produces from a
 * source name because `$` is not in the sanitized alphabet.
 */
Name fieldPartName(JsShared& g, Name field, StringView suffix) {
    auto text = stringView(g.context.findName(field.text));

    char buffer[512];
    auto length = text.length < sizeof(buffer) - 8 ? text.length : sizeof(buffer) - 8;
    copy(text.ptr, buffer, length);
    copy(suffix.ptr, buffer + length, suffix.length);

    return Name { internText(g, StringView { buffer, length + suffix.length }) };
}

/*
 * The property names that are the compiler's rather than the program's - `$tag`, `$p`, `$v`.
 *
 * One function rather than a line in each generator's setup, because the names are the *target's*
 * convention: two pipelines that each spelled a tag would be two hidden classes for one record the
 * day one of them was changed, and there is no diagnostic for that - a value built by one and read
 * by the other simply answers `undefined`.
 */
void nameSharedFields(JsShared& g) {
    g.tagField = literalName(g, jsir::key::tag);
    g.payloadField = literalName(g, jsir::key::payload);
    g.boxField = literalName(g, jsir::key::box);
    g.codeField = literalName(g, jsir::key::code);
    g.envField = literalName(g, jsir::key::env);
    g.headerField = literalName(g, jsir::key::header);
}

} // namespace js
