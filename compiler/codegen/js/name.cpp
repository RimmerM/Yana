#include "build.h"

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
 * Interning a name the generator built rather than one the source contained.
 *
 * Context::addUnqualifiedName keeps the pointer it is given instead of copying: every other caller
 * hands it a slice of source text, which outlives the compilation. Nothing here does, so the text
 * is copied into the string arena first - and only the first time, since a field name is built
 * again at every projection that reads it.
 */
StringId internText(Gen& g, StringView text) {
    auto id = Context::nameHash(text);
    if(g.interned.contains(id)) return id;

    auto storage = (char*)g.context.stringArena.alloc(text.length);
    copy(text.ptr, storage, text.length);

    g.interned.add(id);
    return g.context.addUnqualifiedName(storage, text.length);
}

namespace {

bool identifierPart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$';
}

bool reservedWord(StringView text) {
    static const StringView kReserved[] = {
        "await"_v, "break"_v, "case"_v, "catch"_v, "class"_v, "const"_v, "continue"_v, "debugger"_v,
        "default"_v, "delete"_v, "do"_v, "else"_v, "enum"_v, "export"_v, "extends"_v, "false"_v,
        "finally"_v, "for"_v, "function"_v, "if"_v, "implements"_v, "import"_v, "in"_v,
        "instanceof"_v, "interface"_v, "let"_v, "new"_v, "null"_v, "package"_v, "private"_v,
        "protected"_v, "public"_v, "return"_v, "static"_v, "super"_v, "switch"_v, "this"_v,
        "throw"_v, "true"_v, "try"_v, "typeof"_v, "undefined"_v, "var"_v, "void"_v, "while"_v,
        "with"_v, "yield"_v, "arguments"_v, "eval"_v, "Infinity"_v, "NaN"_v, "BigInt"_v, "Math"_v,
        "Object"_v, "Array"_v, "Map"_v, "String"_v, "Number"_v, "Boolean"_v, "Symbol"_v,
    };

    for(auto word: kReserved) {
        if(word == text) return true;
    }

    return false;
}

/*
 * A source name as a JS identifier.
 *
 * Mapping every operator character to `_` would make `Num(Int).+` and `Num(Int).-` two names that
 * read alike. Spelling each operator out keeps the emitted code debuggable, which §3.6 asks for and
 * is a large part of what the JS target is *for*; uniqueName() is what guarantees distinctness
 * regardless.
 */
Size sanitize(StringView text, char* target, Size capacity) {
    struct Word { char symbol; StringView text; };
    static const Word kWords[] = {
        { '+', "add"_v }, { '-', "sub"_v }, { '*', "mul"_v }, { '/', "div"_v }, { '%', "rem"_v },
        { '=', "eq"_v }, { '<', "lt"_v }, { '>', "gt"_v }, { '!', "not"_v }, { '&', "and"_v },
        { '|', "or"_v }, { '^', "xor"_v }, { '~', "inv"_v }, { '?', "q"_v },
    };

    Size length = 0;
    auto append = [&](char c) {
        // Runs of separators collapse, so `Num(Int).+` does not come out with three of them in a
        // row where the source had punctuation next to punctuation.
        if(c == '_' && (!length || target[length - 1] == '_')) return;
        if(length + 1 < capacity) target[length++] = c;
    };

    if(text.length && text.ptr[0] >= '0' && text.ptr[0] <= '9') target[length++] = '_';

    for(Size i = 0; i < text.length; i++) {
        auto c = text.ptr[i];

        if(identifierPart(c)) {
            append(c);
            continue;
        }

        StringView word;
        for(auto& entry: kWords) {
            if(entry.symbol == c) word = entry.text;
        }

        if(!word.length) {
            append('_');
            continue;
        }

        append('_');
        for(Size j = 0; j < word.length; j++) append(word.ptr[j]);
    }

    while(length && target[length - 1] == '_') length--;
    while(length && target[0] == '_' && length > 1) {
        for(Size i = 1; i < length; i++) target[i - 1] = target[i];
        length--;
    }

    if(!length) target[length++] = '_';
    return length;
}

} // namespace

Name uniqueName(Gen& g, StringView text, bool local) {
    char buffer[512];
    auto length = sanitize(text, buffer, sizeof(buffer) - 16);

    if(reservedWord(StringView { buffer, length })) buffer[length++] = '$';

    // A name already handed out gets a numeric tail. The counter is kept per base name rather than
    // globally, so an identifier that never collides never grows one.
    auto candidate = internText(g, StringView { buffer, length });
    auto taken = [&](StringId id) {
        return g.moduleNames.contains(id) || (local && g.localNames.contains(id));
    };

    if(taken(candidate)) {
        auto base = length;
        for(U32 counter = 1;; counter++) {
            length = base;
            buffer[length++] = '$';
            length += show(U64(counter), buffer + length, sizeof(buffer) - length);

            candidate = internText(g, StringView { buffer, length });
            if(!taken(candidate)) break;
        }
    }

    if(local) {
        g.localNames.add(candidate);
    } else {
        g.moduleNames.add(candidate);
    }

    return Name { candidate };
}

Name generatedName(Gen& g, StringView prefix, U32 index) {
    char buffer[64];
    copy(prefix.ptr, buffer, prefix.length);

    auto length = prefix.length;
    length += show(U64(index), buffer + length, sizeof(buffer) - length);

    return uniqueName(g, StringView { buffer, length }, true);
}

Name valueName(Gen& g, Value& value) {
    if(value.name) {
        auto text = g.context.findName(value.name);
        return uniqueName(g, stringView(text), true);
    }

    return generatedName(g, "v"_v, value.id);
}

/*
 * One part of a flattened narrow reference, named after the reference itself.
 *
 * `flip(p$o, p$k, p$s)` rather than three anonymous slots, so that emitted source still reads as a
 * reference to `p` taken apart rather than as three unrelated parameters. Each part goes through
 * uniqueName like any other local, since the base name it is built from is already taken.
 */
Name refPartName(Gen& g, Value& value, StringView suffix) {
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

Name propertyName(Gen& g, StringView text) {
    char buffer[512];
    auto length = sanitize(text, buffer, sizeof(buffer));
    return Name { internText(g, StringView { buffer, length }) };
}

Name literalName(Gen& g, StringView text) {
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
Name packedWordName(Gen& g, U32 offset) {
    char buffer[32];
    buffer[0] = '$';
    buffer[1] = 'p';

    auto length = 2 + show(U64(offset), buffer + 2, sizeof(buffer) - 2);
    return Name { internText(g, StringView { buffer, length }) };
}

Name fieldName(Gen& g, StringId name, U16 index) {
    if(name) {
        auto text = g.context.findName(name);
        return propertyName(g, stringView(text));
    }

    char buffer[32];
    buffer[0] = 'f';
    auto length = 1 + show(U64(index), buffer + 1, sizeof(buffer) - 1);
    return Name { internText(g, StringView { buffer, length }) };
}

} // namespace js
