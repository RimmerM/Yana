#include "shared.h"
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

Name propertyName(JsShared& g, StringView text) {
    char buffer[512];
    auto length = jsSanitize(text, buffer, sizeof(buffer));
    return Name { internText(g, StringView { buffer, length }) };
}

Name literalName(JsShared& g, StringView text) {
    return Name { internText(g, text) };
}

/*
 * The property names that are the compiler's rather than the program's - `$tag`, `$p`, `$v`.
 *
 * One function rather than a line wherever the state is built, because the names are the
 * *target's* convention: two places that each spelled a tag would be two hidden classes for one
 * record the day one of them was changed, and there is no diagnostic for that - a value built by
 * one and read by the other simply answers `undefined`.
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
