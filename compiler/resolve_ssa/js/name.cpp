#include "name.h"

/*
 * See name.h. What is here is the target's rules for what an identifier may be; what is in
 * `codegen/js/name.cpp` is the uniquing, which needs the file being emitted.
 */
namespace jsir {

bool jsIdentifierPart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$';
}

bool jsReservedWord(StringView text) {
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
Size jsSanitize(StringView text, char* target, Size capacity) {
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

        if(jsIdentifierPart(c)) {
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


StringId jsInternText(Context& context, HashSet<StringId>& interned, StringView text) {
    auto id = Context::nameHash(text);
    if(interned.contains(id)) return id;

    /*
     * A name whose hash already belongs to *other* text.
     *
     * A StringId is the text's 32-bit hash and nothing else, and registering a second text under one
     * replaces the first's - so every name that id stood for, a string literal included, would print
     * as the newcomer. A caller that could have chosen differently is steered off a collision by the
     * uniquing; a property is the text it prints, so it stops here rather than rewriting whatever
     * the colliding id already names.
     */
    auto& known = context.find(id);
    auto clash = known.textLength && !(StringView { known.text, known.textLength } == text);
    assertTrue(!clash);

    auto storage = (char*)context.stringArena.alloc(text.length);
    copy(text.ptr, storage, text.length);

    interned.add(id);
    return context.addUnqualifiedName(storage, text.length);
}

} // namespace jsir
