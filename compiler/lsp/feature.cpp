#include "feature.h"
#include "../resolve/name.h"
#include "../util/lexer_util.h"

namespace lsp {

/*
 * Locations.
 */

LocationWriter::FileLines* LocationWriter::linesOf(StringId module) {
    if(!module) return nullptr;

    for(auto& file: files) {
        if(file->module == module) return file.get();
    }

    auto path = session.pathOf(module);
    if(path.length == 0) return nullptr;

    auto built = Ptr(new FileLines());
    built->module = module;
    built->uri = pathToUri(path);
    built->text = session.provider.getSource(module);
    built->lines.build(built->text);

    auto result = built.get();
    files.push(::move(built));
    return result;
}

// Both ends of one location, as the client counts them.
static void writePosition(Net::JsonWriter& json, const LineTable& lines, StringView text, U32 offset,
                          bool utf16) {
    auto line = lines.lineOf(offset);

    json.startObject();
    json.field("line"_v).value(line);
    json.field("character"_v).value(utf16 ? lines.utf16Column(text, offset) : offset - lines.lineStart(line));
    json.endObject();
}

bool LocationWriter::writeRange(Net::JsonWriter& json, LocationId id) {
    if(!session.context) return false;

    auto location = session.context->getLocation(id);
    if(!location) return false;

    auto file = linesOf(location->sourceModule);
    if(!file) return false;

    json.startObject();
    json.field("start"_v);
    writePosition(json, file->lines, file->text, location->sourceStart.offset, utf16);
    json.field("end"_v);
    writePosition(json, file->lines, file->text, location->sourceEnd.offset, utf16);
    json.endObject();
    return true;
}

bool LocationWriter::writeLocation(Net::JsonWriter& json, LocationId id) {
    if(!session.context) return false;

    auto location = session.context->getLocation(id);
    if(!location) return false;

    auto file = linesOf(location->sourceModule);
    if(!file) return false;

    json.startObject();
    json.field("uri"_v).value(stringView(file->uri));
    json.field("range"_v);
    writeRange(json, id);
    json.endObject();
    return true;
}

bool LocationWriter::writeModuleLocation(Net::JsonWriter& json, StringId module) {
    auto file = linesOf(module);
    if(!file) return false;

    // A module is a file, not a declaration, so what an `import` jumps to is its first character.
    json.startObject();
    json.field("uri"_v).value(stringView(file->uri));
    json.field("range"_v).startObject();
    json.field("start"_v).startObject();
    json.field("line"_v).value(U32(0));
    json.field("character"_v).value(U32(0));
    json.endObject();
    json.field("end"_v).startObject();
    json.field("line"_v).value(U32(0));
    json.field("character"_v).value(U32(0));
    json.endObject();
    json.endObject();
    json.endObject();
    return true;
}

/*
 * Definition.
 */

void writeDefinition(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                     StringId module, U32 offset) {
    auto reference = session.referenceAt(module, offset);

    // A cursor on the declaration itself answers with the declaration - which is what a client does
    // when "go to definition" is pressed twice, and is better than nothing happening.
    auto symbol = reference ? &reference->target : session.definitionAt(module, offset);
    if(!symbol) {
        json.null();
        return;
    }

    if(symbol->kind == Symbol::Kind::Module || symbol->kind == Symbol::Kind::Import) {
        if(symbol->module && locations.writeModuleLocation(json, symbol->module->name)) return;

        json.null();
        return;
    }

    if(symbol->definition == kNullLocation || !locations.writeLocation(json, symbol->definition)) {
        // Everything in Core and Native, which are compiled into the compiler. Answering null says
        // "there is nowhere to go", which is true, rather than pointing at an unrelated file.
        json.null();
    }
}

/*
 * Type definition - §6's `typeDefinition` row.
 *
 * A different question from `definition`, which is what it was answered with before: "go to the
 * declaration of the *type* of what is under the cursor" rather than of the thing itself. On a
 * local `p: Point` the first lands on `data Point` and the second on the `let` that bound it, and
 * an editor offers both because both are wanted.
 *
 * The type at the occurrence is what the semantic index already recorded, and `typeSymbol` is what
 * turns an instantiation back into the declaration it was made from - `Maybe(Int)` jumps to
 * `Maybe`, which is where the source text is.
 */
void writeTypeDefinition(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                         StringId module, U32 offset) {
    auto reference = session.referenceAt(module, offset);
    if(!reference || !reference->type || !reference->target.module) {
        json.null();
        return;
    }

    auto symbol = typeSymbol(*reference->target.module, reference->type);
    if(symbol.definition == kNullLocation || !locations.writeLocation(json, symbol.definition)) {
        json.null();
    }
}

/*
 * References.
 */

void writeReferences(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                     StringId module, U32 offset, bool includeDeclaration) {
    json.startArray();
    if(!session.index) {
        json.endArray();
        return;
    }

    auto reference = session.referenceAt(module, offset);
    auto symbol = reference ? &reference->target : session.definitionAt(module, offset);
    if(!symbol) {
        json.endArray();
        return;
    }

    // Copied rather than borrowed. The symbol points into the same array `findOccurrences` walks,
    // and a comparison target that is an element of what is being compared is a dependency on the
    // walk not touching it - which is true today and not something a caller should have to know.
    auto target = *symbol;

    if(includeDeclaration && target.definition != kNullLocation) {
        json.arrayField();
        if(!locations.writeLocation(json, target.definition)) json.null();
    }

    Array<const Reference*> occurrences;
    session.index->findOccurrences(target, occurrences);

    for(auto occurrence: occurrences) {
        // A binding is recorded as an occurrence of itself, so that its type is written down where
        // it was introduced. It is the declaration rather than a use of it, and the declaration is
        // either already written above or deliberately left out.
        if(occurrence->source == target.definition) continue;

        json.arrayField();
        if(!locations.writeLocation(json, occurrence->source)) json.null();
    }

    json.endArray();
}

/*
 * The protocol's `SymbolKind`, which is what a structure view draws its icon from. Close relatives
 * of `completionItemKind`'s answers and a different numbering, which is the protocol's doing rather
 * than a decision here.
 */
static U32 documentSymbolKind(Symbol::Kind kind) {
    switch(kind) {
        case Symbol::Kind::Function: return 12;    // Function
        case Symbol::Kind::ClassFun: return 6;     // Method
        case Symbol::Kind::Global: return 14;      // Constant
        case Symbol::Kind::Type:
        case Symbol::Kind::Alias: return 23;       // Struct
        case Symbol::Kind::Class: return 11;       // Interface
        case Symbol::Kind::Constructor: return 22; // EnumMember
        default: return 13;                        // Variable
    }
}

/*
 * Document highlights - §6's row: the `references` answer, restricted to one file.
 */

void writeDocumentHighlights(Net::JsonWriter& json, Session& session, StringId module, U32 offset,
                             StringView text, const LineTable& lines, bool utf16) {
    json.startArray();

    if(!session.index || !session.context) {
        json.endArray();
        return;
    }

    auto& context = *session.context;
    auto reference = session.referenceAt(module, offset);
    auto symbol = reference ? &reference->target : session.definitionAt(module, offset);
    if(!symbol) {
        json.endArray();
        return;
    }

    auto target = *symbol;

    // The protocol's own kinds: 3 is a write and 2 a read. Nothing here distinguishes the two yet -
    // a reference does not say whether it was assigned through - so a declaration is a `Write` and
    // every use a `Read`, which is the distinction a reader actually scans for.
    auto write = [&](LocationId id, U32 kind) {
        auto location = context.getLocation(id);
        if(!location || location->sourceModule != module) return;

        auto start = location->sourceStart.offset;
        auto end = location->sourceEnd.offset;
        if(end > text.length || start > end) return;

        json.arrayField().startObject();
        json.field("range"_v).startObject();
        json.field("start"_v);
        writePosition(json, lines, text, start, utf16);
        json.field("end"_v);
        writePosition(json, lines, text, end, utf16);
        json.endObject();
        json.field("kind"_v).value(kind);
        json.endObject();
    };

    write(target.definition, 3);

    Array<const Reference*> occurrences;
    session.index->findOccurrences(target, occurrences);
    for(auto occurrence: occurrences) {
        if(occurrence->source == target.definition) continue;
        write(occurrence->source, 2);
    }

    json.endArray();
}

/*
 * Document symbols - §6's row.
 */

void writeDocumentSymbols(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                          StringId module) {
    json.startArray();

    if(!session.index || !session.context) {
        json.endArray();
        return;
    }

    auto& context = *session.context;

    /*
     * Out of the semantic index's definitions rather than out of the module's declaration tables,
     * which is what §6 named as the source.
     *
     * They are the same set for everything a file declares, and the index has the two things the
     * tables do not: one entry per *written* declaration - a class function is recorded where it was
     * declared rather than once per instance that implements it - and the location already resolved
     * to the file it was written in. What the tables would add is an order, and the order a hash map
     * is walked in is not one, so it is sorted here by where each declaration starts.
     */
    struct Entry {
        const Symbol* symbol;
        U32 start;
    };

    Array<Entry> entries;

    for(auto& symbol: session.index->definitions) {
        switch(symbol.kind) {
            case Symbol::Kind::Function:
            case Symbol::Kind::Global:
            case Symbol::Kind::Type:
            case Symbol::Kind::Alias:
            case Symbol::Kind::Class:
            case Symbol::Kind::ClassFun:
            case Symbol::Kind::Constructor:
                break;
            default:
                continue;
        }

        if(symbol.definition == kNullLocation) continue;

        auto location = context.getLocation(symbol.definition);
        if(!location || location->sourceModule != module) continue;

        entries.push(Entry { &symbol, location->sourceStart.offset });
    }

    for(U32 i = 1; i < entries.size(); i++) {
        auto entry = entries[i];
        auto j = i;
        while(j > 0 && entry.start < entries[j - 1].start) {
            entries[j] = entries[j - 1];
            j--;
        }

        entries[j] = entry;
    }

    for(auto& entry: entries) {
        auto& symbol = *entry.symbol;

        json.arrayField().startObject();
        json.field("name"_v).value(context.findName(symbol.name));

        StringBuilder detail;
        describeSymbol(context, symbol, nullptr, detail);
        if(detail.size()) json.field("detail"_v).value(detail.view());

        json.field("kind"_v).value(documentSymbolKind(symbol.kind));

        // The whole declaration, and then the name inside it. A client uses the first to decide
        // what the caret is inside and the second to decide what to select when it is chosen.
        json.field("range"_v);
        if(!locations.writeRange(json, symbol.definition)) json.null();
        json.field("selectionRange"_v);
        if(!locations.writeRange(json, symbol.definition)) json.null();

        json.endObject();
    }

    json.endArray();
}

/*
 * Folding ranges - §6's row.
 */

void writeFoldingRanges(Net::JsonWriter& json, StringView text, const LineTable& lines) {
    json.startArray();

    // The indentation of each line, and whether it has anything on it. A blank line belongs to
    // whichever block surrounds it rather than closing one, which is also the lexer's rule.
    struct LineInfo {
        U32 indent = 0;
        bool blank = true;
    };

    Array<LineInfo> info;

    for(U32 line = 0; line < lines.lineCount(); line++) {
        auto start = lines.lineStart(line);
        auto end = line + 1 < lines.lineCount() ? lines.lineStart(line + 1) : U32(text.length);

        LineInfo entry;
        auto i = start;
        for(; i < end; i++) {
            auto c = text.ptr[i];
            if(c == ' ') { entry.indent++; continue; }
            if(c == '\t') { entry.indent += 4; continue; }
            if(c == '\n' || c == '\r') break;

            entry.blank = false;
            break;
        }

        info.push(entry);
    }

    for(U32 line = 0; line < info.size(); line++) {
        if(info[line].blank) continue;

        // The next line with anything on it. A header whose block is separated from it by empty
        // lines still opens one - which is the state M7's `withLevel` refuses to *parse*, and folding
        // has no reason to be as strict: an editor still has something to fold.
        U32 next = line + 1;
        while(next < info.size() && info[next].blank) next++;
        if(next >= info.size() || info[next].indent <= info[line].indent) continue;

        // As far as the indentation holds. Blank lines inside the block are part of it and blank
        // lines after it are not, which is why the end is the last non-blank line rather than the
        // first line that ends it.
        U32 end = line;
        for(U32 i = next; i < info.size(); i++) {
            if(info[i].blank) continue;
            if(info[i].indent <= info[line].indent) break;

            end = i;
        }

        if(end <= line) continue;

        json.arrayField().startObject();
        json.field("startLine"_v).value(line);
        json.field("endLine"_v).value(end);
        json.endObject();
    }

    json.endArray();
}

/*
 * Hover.
 */

// The specializations a generic function was cloned for - §1.3's answer to "which type does hover
// show inside a generic body". The generic type, plus what it was instantiated at, so the answer is
// neither a lie nor useless.
static void describeSpecializations(Context& context, Module& module, Function& function,
                                    StringBuilder& into) {
    auto global = *module.types;
    auto local = *module.arena;
    if(!function.gen || function.specializations.isEmpty()) return;

    into << "\nspecialized at: ";

    auto first = true;
    for(auto pointer: function.specializations.contents(local)) {
        auto specialization = local[pointer];
        if(!first) into << "; ";
        first = false;

        auto firstArg = true;
        for(auto type: specialization->genericArgs.contents(local)) {
            if(!firstArg) into << ", ";
            firstArg = false;
            describeType(context, global, type, into);
        }
    }
}

void describeAt(Session& session, StringId module, U32 offset, StringBuilder& into) {
    if(!session.context) return;

    auto& context = *session.context;
    auto reference = session.referenceAt(module, offset);
    auto symbol = reference ? &reference->target : session.definitionAt(module, offset);
    if(!symbol) return;

    describeSymbol(context, *symbol, reference ? reference->type : nullptr, into);

    if(symbol->module && symbol->module->name != module) {
        into << "\n-- from ";
        into << context.findName(symbol->module->name);
    }

    // Which instance served this call, which is the question a class function's signature does not
    // answer and the reason §1.2 records the selection rather than the candidate set.
    if(reference && reference->instance && symbol->module) {
        auto local = *symbol->module->arena;
        auto global = *symbol->module->types;
        auto instance = local[reference->instance];

        into << "\n-- instance ";
        into << context.findName(global[instance->typeClass]->name);
        into << "(";

        auto first = true;
        for(auto type: instance->forTypes.contents(local)) {
            if(!first) into << ", ";
            first = false;
            describeType(context, global, type, into);
        }

        into << ")";
    }

    if(symbol->kind == Symbol::Kind::Function && symbol->module) {
        describeSpecializations(context, *symbol->module,
                                *(*symbol->module->arena)[ModulePtr<Function>(symbol->payload)], into);
    }
}

/*
 * The `explain` section of a hover - Implementation-Tooling.md M9, §7.2.
 *
 * The record and the printer both exist; this is the caller §7 says was missing. What it adds is
 * the five inferred properties that are invisible in the source - storage, mutation demand,
 * retention, the return root, and how the function is reached - and only where one of them is
 * *surprising*, which is `printExplanationHover`'s own filter rather than a second one here.
 *
 * A function only, because that is what an explanation is about. A cursor on a call gets the
 * callee's, which is the useful direction: what this call is about to cost is a property of the
 * thing being called.
 */
bool explainAt(Session& session, StringId module, U32 offset, StringBuilder& into) {
    if(!session.context || !session.program) return false;

    auto reference = session.referenceAt(module, offset);
    auto symbol = reference ? &reference->target : session.definitionAt(module, offset);
    if(!symbol || symbol->kind != Symbol::Kind::Function || !symbol->module) return false;

    auto function = (*symbol->module->arena)[ModulePtr<Function>(symbol->payload)];
    if(!function) return false;

    auto explanation = explainFunction(*session.program, *function, session.callSites());
    return printExplanationHover(into, *session.context, *session.program, explanation);
}

void writeHover(Net::JsonWriter& json, Session& session, LocationWriter& locations,
                StringId module, U32 offset) {
    StringBuilder text;
    describeAt(session, module, offset, text);

    if(text.size() == 0) {
        json.null();
        return;
    }

    // A fenced block rather than plain text: what is in it is Yana source, and a client that
    // renders markdown will highlight it with the same grammar the editor uses.
    StringBuilder markdown;
    markdown << "```yana\n" << text.view() << "\n```";

    // Underneath the fence rather than inside it, because what it holds is markdown and not Yana -
    // a list of what the compiler inferred, which is the whole point of M9.
    explainAt(session, module, offset, markdown);

    json.startObject();
    json.field("contents"_v).startObject();
    json.field("kind"_v).value("markdown"_v);
    json.field("value"_v).value(markdown.view());
    json.endObject();

    // The range the answer is about, so the client can highlight exactly what it asked over.
    if(auto reference = session.referenceAt(module, offset)) {
        json.field("range"_v);
        if(!locations.writeRange(json, reference->source)) json.null();
    }

    json.endObject();
}

/*
 * Inlay hints - Implementation-Tooling.md §6's `inlayHint` row and M9.
 *
 * Two things the source does not say and the compiler knows: what a binding's type came out as when
 * nobody wrote one, and what `explain` found surprising about a function. Both are read off what the
 * compile already left - the semantic index for the bindings, `explainFunction` for the functions -
 * so neither is an analysis and neither costs more than a walk of the definitions.
 *
 * The protocol places a hint *at* a position and a client renders it inline there, so `explain`'s
 * "a line above each fn" (§7.3) becomes the end of the declaration's own line. That is the same
 * information in the place the protocol has for it, and it keeps the hint beside the signature it
 * is about rather than above a blank line.
 */
void writeInlayHints(Net::JsonWriter& json, Session& session, StringId module, StringView text,
                     const LineTable& lines, bool utf16, U32 from, U32 to) {
    json.startArray();

    if(!session.index || !session.context || !session.program) {
        json.endArray();
        return;
    }

    auto& context = *session.context;

    // The end of the line a declaration starts on, which is where a hint about the whole
    // declaration belongs: a signature's own range ends before the `=` of a one-line body, and a
    // hint written there would sit in the middle of what it is about.
    auto endOfLine = [&](U32 offset) {
        auto line = lines.lineOf(offset);
        auto end = line + 1 < lines.lineCount() ? lines.lineStart(line + 1) : U32(text.length);
        while(end > offset && (text.ptr[end - 1] == '\n' || text.ptr[end - 1] == '\r')) end--;
        return end;
    };

    auto position = [&](U32 offset) {
        writePosition(json, lines, text, offset, utf16);
    };

    for(auto& symbol: session.index->definitions) {
        if(symbol.definition == kNullLocation) continue;

        auto location = context.getLocation(symbol.definition);
        if(!location || location->sourceModule != module) continue;

        auto start = location->sourceStart.offset;
        auto end = location->sourceEnd.offset;
        if(start < from || start >= to || end > text.length) continue;

        StringBuilder label;
        U32 at = 0;

        if(symbol.kind == Symbol::Kind::Function && symbol.module) {
            auto function = (*symbol.module->arena)[ModulePtr<Function>(symbol.payload)];
            if(!function) continue;

            auto explanation = explainFunction(*session.program, *function, session.callSites());
            auto summary = explanationSummary(context, *session.program, explanation);
            if(summary.size() == 0) continue;

            label << "-- " << stringView(summary);
            at = endOfLine(start);
        } else if(symbol.kind == Symbol::Kind::Local && symbol.module) {
            // The type comes off the occurrence recorded at the declaration rather than off the
            // slot: an immutable binding names an SSA value and has no slot to read one from.
            auto declaration = session.index->findReference(symbol.definition);
            if(!declaration || !declaration->type || !symbol.name) continue;

            /*
             * Only where the author did not write the type. Whether they did is a question about
             * the text rather than about the slot - a written type and an inferred one produce the
             * same `Local` - so the answer is the next thing after the name, which is a `:` exactly
             * when one was written.
             */
            auto after = end;
            while(after < text.length && (text.ptr[after] == ' ' || text.ptr[after] == '\t')) after++;
            if(after < text.length && text.ptr[after] == ':') continue;

            label << ": ";
            describeType(context, *symbol.module->types, declaration->type, label);
            at = end;
        } else {
            continue;
        }

        if(at < from || at > to) continue;

        json.arrayField().startObject();
        json.field("position"_v);
        position(at);
        json.field("label"_v).value(label.view());

        // A type hint is the protocol's own `Type` kind, which is what a client themes and what its
        // "show type hints" setting switches; an `explain` line is neither a type nor a parameter
        // name, so it carries no kind and is drawn as a plain hint.
        if(symbol.kind == Symbol::Kind::Local) json.field("kind"_v).value(U32(1));

        // Padding on the side the text is not already spaced from, so a hint reads as part of the
        // line rather than as something glued to the name before it.
        if(symbol.kind == Symbol::Kind::Function) json.field("paddingLeft"_v).value(true);

        json.endObject();
    }

    json.endArray();
}

/*
 * Completion - Implementation-Tooling.md §8.
 */

/*
 * The protocol's `CompletionItemKind`, which is what a client draws the icon from.
 *
 * A constructor is an `EnumMember` rather than the protocol's own `Constructor`, on the same terms
 * as the semantic token in §11: Yana's constructors are the cases of a sum type, and every editor
 * already has an icon that means exactly that. A class is an `Interface`, because what a typeclass
 * is to a reader is a set of operations a type may implement.
 */
U32 completionItemKind(Symbol::Kind kind) {
    switch(kind) {
        case Symbol::Kind::Function: return 3;   // Function
        case Symbol::Kind::ClassFun: return 2;   // Method
        case Symbol::Kind::Constructor: return 20; // EnumMember
        case Symbol::Kind::Type:
        case Symbol::Kind::Alias: return 22;     // Struct
        case Symbol::Kind::Class: return 8;      // Interface
        case Symbol::Kind::Field: return 5;      // Field
        case Symbol::Kind::TypeVar: return 25;   // TypeParameter
        case Symbol::Kind::Module:
        case Symbol::Kind::Import: return 9;     // Module
        default: return 6;                       // Variable - the three local kinds and a global
    }
}

// Case-insensitive, because that is what a client's own filter does and a server that is stricter
// than the client hides items the client would have kept. ASCII only: a name outside it is compared
// byte for byte, which is what the lexer's own identifier rules already restrict names to.
static bool startsWith(StringView name, StringView prefix) {
    if(prefix.length > name.length) return false;

    for(Size i = 0; i < prefix.length; i++) {
        auto a = name.ptr[i], b = prefix.ptr[i];
        if(a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if(b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if(a != b) return false;
    }

    return true;
}

/*
 * What selecting an item actually types.
 *
 * A name on its own is only ever half of what the author meant: choosing a function is choosing to
 * call it, and choosing a constructor is choosing to build one. So an item that takes arguments
 * inserts its brackets and puts the caret in the first of them, which is the difference between
 * completion finishing a thought and completion saving four keystrokes.
 *
 * As a snippet where the client accepts one - `f(${1:a}, ${2:b})` - so the caret lands on the first
 * argument and Tab walks the rest. A client that declined `snippetSupport` gets the bare name for
 * anything with arguments, rather than the placeholder syntax as literal text: the fallback has to
 * be worse, not wrong.
 *
 * Returns true when what it wrote is a snippet.
 */
/*
 * What the document around the caret says about what may be inserted.
 *
 * Every one of these is a question about the text rather than about the item: whether the brackets
 * an item would open are already there, whether the `:` a field name needs has already been typed,
 * and whether the answer is a field of something being *written* - which is what decides whether a
 * field name completes to `side` or to `side: `. A field offered after a `.` is a projection and
 * takes no colon; the same field offered inside `Square {` is half of a `field: value` pair.
 */
struct InsertSite {
    bool snippets = false;
    bool constructing = false;
    bool bracketsWritten = false;
    bool colonWritten = false;

    // Set per *item* rather than per request: an argument position offers the call's parameter names
    // and the names in scope together, and only the first half is the name of a pair. See
    // CompletionItem::naming.
    bool naming = false;
};

static bool writeInsertText(Context& context, const Symbol& symbol, const String& label,
                            const InsertSite& site, StringBuilder& into) {
    into << label;

    auto module = symbol.module;
    if(!module) return false;

    auto global = *module->types;
    auto local = *module->arena;
    auto snippets = site.snippets;

    /*
     * A field name inside the braces of a construction, which is half of what the author meant in
     * the same way a function name is: choosing a field is choosing to give it a value.
     *
     * Plain text rather than a snippet, because there is nothing to place a caret *in* - the caret
     * ends up after the space, which is where the value goes. So this is the one insertion a client
     * without `snippetSupport` gets in full.
     */
    if(symbol.kind == Symbol::Kind::Field) {
        if(site.constructing && !site.colonWritten) into << ": ";
        return false;
    }

    // A parameter name at an argument position, which is the same half of the same shape - `f(mode:
    // ` - reached through the item rather than through the request. See InsertSite::naming.
    if(site.naming) {
        if(!site.colonWritten) into << ": ";
        return false;
    }

    // A placeholder per written thing: `${n:name}`, or `${n}` where there is no name worth showing.
    // The number goes through `format` rather than `<<`, whose only integral overload is `char`.
    U32 index = 0;
    auto placeholder = [&](StringId name) {
        char digits[12];
        into << "${";
        into.append(digits, format(toBuffer(digits), toString("%@"_v), ++index));

        if(name) into << ":" << context.findName(name);
        into << "}";
    };

    /*
     * A function, and the class functions that are called exactly like one.
     */
    if(symbol.kind == Symbol::Kind::Function || symbol.kind == Symbol::Kind::ClassFun) {
        Function* function = nullptr;

        if(symbol.kind == Symbol::Kind::Function) {
            function = local[ModulePtr<Function>(symbol.payload)];
        } else {
            auto typeClass = global[GlobalPtr<TypeClass>(symbol.payload)];
            if(symbol.index >= typeClass->functions.size()) return false;

            auto entry = typeClass->functions.get(global, symbol.index);
            if(!entry.fun) return false;

            function = local[entry.fun];
        }

        /*
         * The positions the call site has to write, which is not every parameter.
         *
         * A trailing parameter with a default is not one, for the reason the constructor case below
         * gives about a field with one: a placeholder for something the author did not have to
         * mention is one they have to delete. *Trailing* only - a defaulted position in the middle
         * cannot be dropped from a positional call, since what follows it would move up a place, and
         * skipping one is what a named argument is for rather than what a snippet should assume.
         */
        auto written = function->args.size();
        while(written && local[function->args.get(local, written - 1)]->hasDefault()) written--;

        // A call with nothing to write is finished by the empty brackets, so it needs no snippet -
        // and a client without snippet support gets the same text, which is why this is ahead of the
        // check below rather than inside it.
        if(written == 0) {
            into << "()";
            return false;
        }

        if(!snippets) return false;

        into << "(";
        for(Size i = 0; i < written; i++) {
            if(index) into << ", ";
            placeholder(local[function->args.get(local, i)]->name);
        }

        into << ")";
        return true;
    }

    /*
     * A constructor, whose brackets say which of the two forms it was declared in - `Just(x)` for a
     * payload the declaration did not name, and `Square {side: x}` for one it did. `Nothing` carries
     * nothing and is complete as it stands.
     */
    if(symbol.kind == Symbol::Kind::Constructor) {
        auto record = global[GlobalPtr<RecordType>(symbol.payload)];
        if(symbol.index >= record->constructors.size()) return false;

        // `Nothing` is complete as it stands. Its payload is not absent but *empty* - Core writes it
        // as `Maybe.Nothing(())` - so the test is unit rather than null, and a constructor whose
        // payload carries nothing takes no brackets.
        auto content = record->constructors.get(global, symbol.index).content;
        if(!content || isUnit(global, content)) return false;

        if(!snippets) return false;

        if(global[content]->kind != Type::Tup) {
            into << "(";
            placeholder(0);
            into << ")";
            return true;
        }

        auto tuple = (TupType*)global[content];
        if(tuple->fields.size() == 0) return false;

        /*
         * Which fields have to be written, which is the same question a call asks of its arguments.
         *
         * A field with a declared default is not one of them: `Square {}` is a complete expression
         * when every field has one, and a placeholder for a field the author did not have to
         * mention is one they have to delete. The defaults are read from the *declaration* rather
         * than from this record, which may be an instantiation of it - the same reason
         * resolveConstruct gives, and the same `base()` it goes through.
         */
        auto declaration = (RecordType*)global[record->base(global)];
        auto defaults = symbol.index < declaration->constructors.size()
                      ? declaration->constructors.get(global, symbol.index).defaults
                      : GlobalList<FieldDefault>();

        auto defaulted = [&](U16 field) {
            for(auto entry: defaults.contents(global)) {
                if(entry.field == field) return true;
            }

            return false;
        };

        U32 required = 0;
        for(Size i = 0; i < tuple->fields.size(); i++) required += defaulted(U16(i)) ? 0 : 1;

        // Nothing left to write: every field carries its own value, so the name alone is the whole
        // expression and brackets would only be something to fill in or remove.
        if(required == 0) return false;

        // Named fields are written as a record and positional ones as arguments, which is the same
        // distinction the parser makes after a ConID.
        auto named = tuple->fields.get(global, 0).name != 0;
        into << (named ? " {" : "(");

        for(Size i = 0; i < tuple->fields.size(); i++) {
            auto field = tuple->fields.get(global, i);

            // A positional payload has no name to write, so a defaulted field in the middle of one
            // cannot be skipped - what follows it would move up a place. Only the named form, where
            // every value says which field it is.
            if(named && defaulted(U16(i))) continue;

            if(index) into << ", ";

            if(named) into << context.findName(field.name) << ": ";
            placeholder(named ? StringId(0) : field.name);
        }

        into << (named ? "}" : ")");
        return true;
    }

    return false;
}

/*
 * Whether the position is already followed by the brackets an item would insert.
 *
 * Completing `pick` in `pick(1, 2)` must not produce `pick(a, b)(1, 2)`, and what tells the two
 * apart is the next thing in the document rather than anything about the item.
 *
 * Past the rest of the name first, because the caret is usually *inside* the name being edited and
 * the client replaces the whole word rather than the part before the caret. Looking only at what
 * follows the caret would see the tail of the name and conclude there was no call.
 */
static bool followedByBrackets(StringView text, U32 offset) {
    auto i = Size(offset);

    while(i < text.length && isIdentifier(text.ptr[i])) i++;
    while(i < text.length && (text.ptr[i] == ' ' || text.ptr[i] == '\t')) i++;

    return i < text.length && (text.ptr[i] == '(' || text.ptr[i] == '{');
}

// And whether the `:` a field name would insert is already written. `Square {si|: 3}` is a field
// being *edited*, and the same completion there must not produce `side: : 3`.
static bool followedByColon(StringView text, U32 offset) {
    auto i = Size(offset);

    while(i < text.length && isIdentifier(text.ptr[i])) i++;
    while(i < text.length && (text.ptr[i] == ' ' || text.ptr[i] == '\t')) i++;

    return i < text.length && text.ptr[i] == ':';
}

void writeCompletion(Net::JsonWriter& json, Session& session, StringId module, U32 offset,
                     StringView text, bool snippets, bool utf16) {
    CompletionRequest request;
    U32 prefixStart = offset;

    // The compile is the answer: the sentinel goes in during the parse, and everything completion
    // reads is what the resolver had in hand when it reached one.
    session.complete(module, offset, request, prefixStart);

    // After the compile, never before: a compile drops every buffer the provider had loaded and
    // reads them again, so a view taken from it beforehand points at freed memory. An open
    // document's text is the caller's own and survives, which is why it is passed in at all.
    if(text.length == 0) text = session.provider.getSource(module);

    // Clamped against the text rather than trusted, because the two need not agree: a client may
    // name a position in a version of the document that this compile did not read.
    if(offset > text.length) offset = U32(text.length);
    if(prefixStart > offset) prefixStart = offset;

    StringView prefix { text.ptr + prefixStart, offset - prefixStart };

    /*
     * A qualified name is one token, so the prefix under the cursor may be `Shapes.Ci` rather than
     * `Ci` - and what is being completed is the last segment of it.
     *
     * Two things follow. The filter is the last segment, because that is what the client's own word
     * definition will replace; and the label is the bare name rather than the qualified one, because
     * inserting the qualifier a second time is what would otherwise happen.
     *
     * What this does *not* do is narrow the answer to the module the qualifier names: everything
     * visible is still offered, filtered by the segment. Narrowing needs the qualifier as a
     * resolved module rather than as text, which is a question about the name the sentinel replaced
     * and therefore something the parser would have to carry.
     */
    auto qualifierTyped = false;
    for(Size i = prefix.length; i > 0; i--) {
        if(prefix.ptr[i - 1] != '.') continue;

        prefix = StringView { prefix.ptr + i, prefix.length - i };
        prefixStart += U32(i);
        qualifierTyped = true;
        break;
    }

    /*
     * What the text around the caret allows - see InsertSite. `construct` comes from the compile
     * rather than from the text: whether a field is a projection or half of a `field: value` pair is
     * decided by the position the sentinel was reached at, and the two look the same from here.
     */
    InsertSite site;
    site.snippets = snippets;
    site.constructing = request.construct;
    site.bracketsWritten = followedByBrackets(text, offset);
    site.colonWritten = followedByColon(text, offset);

    auto alreadyCalled = site.bracketsWritten;

    /*
     * What an item replaces, as a range rather than as a guess.
     *
     * `insertText` alone leaves the *range* to the client's own idea of a word, and the two do not
     * always agree - a qualified name is one token here and three words to an editor, and a caret in
     * the middle of a name is a word whose tail the client may or may not take. A `textEdit` says
     * exactly what is being replaced, and it is what the IntelliJ client reads the completion prefix
     * out of, so the two cannot disagree about which characters the answer was filtered against.
     *
     * From the start of the segment being typed to the end of the name the caret is in, which is why
     * this reads past the caret: completing `sca` in `scaled` must replace the whole word rather
     * than leave `led` behind it.
     */
    auto replaceEnd = offset;
    while(replaceEnd < text.length && isIdentifier(text.ptr[replaceEnd])) replaceEnd++;

    LineTable lines;
    lines.build(text);

    auto writeEditRange = [&](Net::JsonWriter& out) {
        out.startObject();
        out.field("start"_v);
        writePosition(out, lines, text, prefixStart, utf16);
        out.field("end"_v);
        writePosition(out, lines, text, replaceEnd, utf16);
        out.endObject();
    };

    // A range that crosses a line break is one no client accepts, and the only way to get one here
    // is a document the compile and the request disagree about.
    auto editable = lines.lineOf(prefixStart) == lines.lineOf(replaceEnd);

    json.startObject();

    // A filtered list is an incomplete one, and saying so is what makes a client ask again when the
    // prefix changes rather than re-filtering a list that was never the whole set.
    json.field("isIncomplete"_v).value(prefix.length != 0);
    json.field("items"_v).startArray();

    // A compile always leaves a context behind, so this is a guard against a session that never
    // opened rather than one against a failed compile.
    if(!session.context) {
        json.endArray();
        json.endObject();
        return;
    }

    auto& context = *session.context;

    for(auto& item: request.items) {
        auto& identifier = context.find(item.symbol.name);
        StringView name { identifier.text, identifier.textLength };
        if(!startsWith(name, prefix)) continue;

        // The name as it has to be *written* here, which for a qualified import is not the name the
        // symbol has. Offering `length` for something only reachable as `C.length` would complete
        // to a program that does not resolve.
        StringBuilder label;
        if(item.qualifier && !qualifierTyped) label << context.findName(item.qualifier) << ".";
        label << name;

        StringBuilder detail;
        describeSymbol(context, item.symbol, item.type, detail);

        json.arrayField().startObject();
        json.field("label"_v).value(label.view());
        json.field("kind"_v).value(completionItemKind(item.symbol.kind));
        if(detail.size()) json.field("detail"_v).value(detail.view());

        // The site is the document's answer plus this item's own: whether choosing it writes the
        // name of a pair is a property of the item, since an argument position offers both kinds.
        auto itemSite = site;
        itemSite.naming = item.naming;

        StringBuilder insert;
        auto isSnippet = !alreadyCalled &&
                         writeInsertText(context, item.symbol, label.view(), itemSite, insert);

        // Only when it differs from the label. An item whose insert text is its own name says
        // nothing by carrying one, and a client is entitled to show the two apart.
        auto inserts = !alreadyCalled && insert.size() != label.size();
        if(inserts) {
            json.field("insertText"_v).value(insert.view());
            json.field("insertTextFormat"_v).value(U32(isSnippet ? 2 : 1));
        }

        // The same text as an edit, for a client that reads one - which is most of them, and all of
        // the ones that do anything about the range. `insertText` stays for the rest: the two say
        // the same thing and the protocol lets a client choose.
        if(editable) {
            json.field("textEdit"_v).startObject();
            json.field("range"_v);
            writeEditRange(json);
            json.field("newText"_v).value(inserts ? insert.view() : label.view());
            json.endObject();
        }

        // The rank, which is the one thing a client sorts by. A digit in front of the label is the
        // whole of it: there are four groups, and inside one the label's own order is what the
        // items were already collected in.
        StringBuilder sort;
        sort << char('0' + item.rank);
        sort << label.view();
        json.field("sortText"_v).value(sort.view());

        json.endObject();
    }

    json.endArray();
    json.endObject();
}

/*
 * Signature help - Implementation-Tooling.md §6.
 */

/*
 * The call the cursor is inside, read out of the document text.
 *
 * §8.5 found this through the position index - the innermost enclosing *node* whose text opens a
 * bracket still unclosed at the cursor - on the grounds that `parseChain` builds the `App` whether
 * or not the closing bracket arrived. That is true of a call whose arguments are written and false
 * of the one an editor is actually asked about: `pick(` at the end of a line is where the overlay
 * has to appear, and there is no node covering the cursor there at all, because the bracket M7's
 * recovery reports is the end of what was parsed. Measured over a file whose only call was being
 * typed, the node-based version answered at none of its 107 positions.
 *
 * So this scans instead, and it scans *forwards from the start of the file* rather than backwards
 * from the cursor. Forwards is what makes strings and comments skippable at all - a scan going
 * backwards cannot tell an opening quote from a closing one - and the whole file is what makes the
 * answer independent of where any node happens to begin. It is one pass over the document per
 * request, which at a keystroke's budget is nothing.
 *
 * `open` is where the bracket is, `bracket` which one it is, and `argument` how many separators at
 * that depth precede the cursor - which is the parameter the caret is in.
 */
struct EnclosingCall {
    U32 open = 0;
    char bracket = 0;
    U32 argument = 0;

    // Where the argument the caret is in begins - just past the bracket, or just past the separator
    // before it. What a named argument's name is read out of; see writtenArgumentName.
    U32 argumentStart = 0;

    bool found = false;
};

// The lexer's own rules for what is not code, in the order it applies them - `parse/lexer.cpp`'s
// skipWhitespace. `{-` has to be tested before `{` is taken for a bracket, or a block comment opens
// a call that never closes.
static bool isCommentOpen(StringView text, U32 i) {
    return i + 2 < text.length && text.ptr[i] == '{' && text.ptr[i + 1] == '-' && text.ptr[i + 2] != '>';
}

// Every bracket still open at the cursor, outermost first. More than the innermost, because the
// innermost is routinely not a call: a bare tuple has no name in front of it, and neither has a
// parenthesized expression, and what somebody writing one wants to see is the call it is an
// argument of. See writeSignatureHelp, which walks this inwards-out.
static void findOpenBrackets(StringView text, U32 offset, Array<EnclosingCall>& stack) {
    if(offset > text.length) offset = U32(text.length);

    for(U32 i = 0; i < offset; i++) {
        auto c = text.ptr[i];

        // A line comment. `!isSymbol` is the lexer's own test, which is what keeps `-->` from
        // starting one.
        if(c == '-' && i + 2 < text.length && text.ptr[i + 1] == '-' && !isSymbol(text.ptr[i + 2])) {
            while(i < offset && text.ptr[i] != '\n') i++;
            continue;
        }

        if(isCommentOpen(text, i)) {
            U32 level = 1;
            for(i += 2; i < offset && level; i++) {
                if(isCommentOpen(text, i)) level++;
                else if(i + 1 < text.length && text.ptr[i] == '-' && text.ptr[i + 1] == '}') {
                    level--;
                    i++;
                }
            }

            continue;
        }

        if(c == '"' || c == '\'') {
            for(i++; i < offset && text.ptr[i] != c; i++) {
                if(text.ptr[i] == '\\') i++;
            }

            continue;
        }

        /*
         * A declaration ends whatever was left open above it.
         *
         * Layout says so: a line that continues an expression is indented under it, so a character
         * at column 0 is a new declaration and nothing before it can still be being written. Without
         * this, one unclosed bracket makes every position below it - to the end of the file - look
         * like it is inside that call, which is the state a file is in for as long as it takes to
         * type the rest of one. Measured on the mid-edit fixture: 183 positions answered instead of
         * the 18 that are actually in a call.
         */
        if(c == '\n' && i + 1 < offset && text.ptr[i + 1] != ' ' && text.ptr[i + 1] != '\t' &&
           text.ptr[i + 1] != '\n' && text.ptr[i + 1] != '\r') {
            stack.clear();
            continue;
        }

        if(c == '(' || c == '[' || c == '{') {
            stack.push(EnclosingCall { i, c, 0, i + 1, true });
        } else if(c == ')' || c == ']' || c == '}') {
            if(stack.size()) stack.pop();
        } else if(c == ',' && stack.size()) {
            auto& entry = stack[stack.size() - 1];
            entry.argument++;
            entry.argumentStart = i + 1;
        }
    }
}

/*
 * The name a call was written against: the identifier run immediately before its bracket.
 *
 * Read backwards from the bracket rather than forwards from a node, since there is no node in the
 * state this is asked in. A callee that is not a plain name - a field, a parenthesized expression,
 * a bare tuple with no constructor in front of it - has no overload set to show, and refusing to
 * read one is what keeps this from guessing.
 */
static bool calleeName(StringView text, const EnclosingCall& call, StringView& into) {
    auto end = call.open;
    while(end > 0 && (text.ptr[end - 1] == ' ' || text.ptr[end - 1] == '\t')) end--;

    auto start = end;
    while(start > 0 && (isIdentifier(text.ptr[start - 1]) || text.ptr[start - 1] == '.')) start--;

    // A qualifier's dot is part of the name and a leading one is not: `.field: 1` inside a record
    // update is a path rather than a callee.
    while(start < end && text.ptr[start] == '.') start++;
    if(start >= end) return false;

    // A number is not a name, and neither is anything a digit begins.
    auto first = text.ptr[start];
    if(first >= '0' && first <= '9') return false;

    /*
     * A declaration's own brackets are not a call, and the keyword in front of the name is what
     * says so: `fn quiet(n: Int)` reads exactly like a call to `quiet` and is the place a signature
     * for it would be least useful. The same for the fields a `data` declares.
     *
     * Only the keyword immediately before the name, rather than anything about the enclosing
     * declaration - a body inside an `instance` is full of ordinary calls, so asking which
     * declaration the cursor is in would have to answer "the innermost one", and this is the same
     * answer read off two words of text.
     */
    auto keywordEnd = start;
    while(keywordEnd > 0 && (text.ptr[keywordEnd - 1] == ' ' || text.ptr[keywordEnd - 1] == '\t')) keywordEnd--;

    auto keywordStart = keywordEnd;
    while(keywordStart > 0 && isIdentifier(text.ptr[keywordStart - 1])) keywordStart--;

    StringView keyword { text.ptr + keywordStart, keywordEnd - keywordStart };
    if(keyword == "fn"_v || keyword == "data"_v || keyword == "class"_v || keyword == "instance"_v ||
       keyword == "alias"_v || keyword == "foreign"_v) {
        return false;
    }

    into = StringView { text.ptr + start, end - start };
    return true;
}

// One candidate, written as the protocol's SignatureInformation. The label is `describeSymbol`'s,
// so a signature shown here and the same one shown by hover are one string built once.
static void writeSignature(Net::JsonWriter& json, Context& context, const Symbol& symbol) {
    StringBuilder label;
    Array<SignatureParameter> parameters;
    describeSymbol(context, symbol, nullptr, label, &parameters);

    json.arrayField().startObject();
    json.field("label"_v).value(label.view());
    json.field("parameters"_v).startArray();

    for(auto& parameter: parameters) {
        json.arrayField().startObject();

        // The range form rather than the string form: a label a client has to find by searching its
        // own signature text picks the wrong one whenever two parameters read the same.
        json.field("label"_v).startArray();
        json.arrayField().value(parameter.start);
        json.arrayField().value(parameter.end);
        json.endArray();
        json.endObject();
    }

    json.endArray();
    json.endObject();
}

// How many parameters a candidate declares, so that the one the written call could still fit is the
// one shown first.
static Size parameterCount(Context& context, const Symbol& symbol) {
    StringBuilder label;
    Array<SignatureParameter> parameters;
    describeSymbol(context, symbol, nullptr, label, &parameters);

    return parameters.size();
}

// Which parameter of a candidate a name reaches, or the count where none does - which is what makes
// "no such parameter" sort a candidate after the ones that have it.
static Size parameterNamed(Context& context, const Symbol& symbol, StringId name) {
    StringBuilder label;
    Array<SignatureParameter> parameters;
    describeSymbol(context, symbol, nullptr, label, &parameters);

    for(Size i = 0; i < parameters.size(); i++) {
        if(parameters[i].name == name) return i;
    }

    return parameters.size();
}

/*
 * The name the argument the caret is in was written with - the `mode` of `open(path, mode: |)`.
 *
 * Read out of the text rather than out of a node, for the reason the whole of this feature is: the
 * document an editor asks about is usually not a program, and `f(mode: ` has no argument to have a
 * name. So it is the same shape the scan already answers with - whitespace, an identifier,
 * whitespace, and a `:` that is not the `::` of an ascription - applied once, to the one argument
 * the caret turned out to be in.
 *
 * Zero where the argument is positional, which is every argument of every call that does not use
 * names and is therefore the answer this must be cheap about.
 */
static bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static StringId writtenArgumentName(Context& context, StringView text, U32 from, U32 to) {
    auto i = from;
    while(i < to && isBlank(text.ptr[i])) i++;

    auto start = i;
    while(i < to && isIdentifier(text.ptr[i])) i++;
    if(i == start) return 0;

    auto end = i;
    while(i < to && isBlank(text.ptr[i])) i++;

    // `::` is an ascription and `:` opens a block, so the one that names an argument is a single
    // colon with something after it that is not another one.
    if(i >= to || text.ptr[i] != ':') return 0;
    if(i + 1 < text.length && text.ptr[i + 1] == ':') return 0;

    return context.addUnqualifiedName(text.ptr + start, end - start);
}

void writeSignatureHelp(Net::JsonWriter& json, Session& session, StringId module, U32 offset,
                        StringView text) {
    auto program = session.program.get();
    Module* owner = program ? program->findModule(module) : nullptr;

    if(!owner || !session.context) {
        json.null();
        return;
    }

    auto& context = *session.context;

    Array<EnclosingCall> open;
    findOpenBrackets(text, offset, open);

    Array<Symbol> candidates;
    EnclosingCall call;

    /*
     * The overload set, through the compiler's own lookup.
     *
     * `findFunction`, `findClassFunctions` and `findConstructor` are what a call site asks, so what
     * is shown is what the call would select from rather than a second opinion about visibility -
     * §8.1's argument for `forEachVisible`, applied to one name instead of all of them.
     * `kNullLocation` keeps the lookup out of the semantic index: this is a question about the
     * program, not an occurrence in it.
     *
     * Innermost first and outwards until something answers, for the same reason `referenceAt` walks
     * outwards: the bracket the caret is in is often not one anything is named in front of - a bare
     * tuple, a parenthesized expression, a subscript - and what its author wants to see then is the
     * call it is an argument of. A subscript is skipped rather than looked up, since the name in
     * front of one is an array and not a callee.
     */
    for(Size i = open.size(); i > 0 && candidates.size() == 0; i--) {
        auto& candidate = open[i - 1];
        StringView name;

        if(candidate.bracket == '[') continue;
        if(!calleeName(text, candidate, name)) continue;

        auto id = findLastChar(name, '.')
                ? context.addQualifiedName(name.ptr, name.length)
                : context.addUnqualifiedName(name.ptr, name.length);

        if(auto function = findFunction(*owner, id, kNullLocation)) {
            candidates.push(functionSymbol(*owner, function));
        }

        ClassFunList overloads;
        findClassFunctions(*owner, id, kNullLocation, overloads);
        for(auto& overload: overloads) {
            candidates.push(classFunSymbol(*owner, overload.typeClass, overload.index));
        }

        /*
         * A constructor, which is the other thing written with brackets and arguments in front of a
         * caret - `Square {side: 3}` and `Just(x)`. §8.5 left it out and there is no reason for it
         * to be: what a record's fields are is exactly what somebody halfway through writing one
         * wants, and it is the same question about the same brackets.
         */
        if(auto constructor = findConstructor(*owner, id, kNullLocation)) {
            candidates.push(constructorSymbol(*owner, constructor.unwrap()));
        }

        call = candidate;
    }

    if(candidates.size() == 0) {
        json.null();
        return;
    }

    /*
     * The first candidate the written call could still fit.
     *
     * A *name* is what to go on where the author wrote one, and it is a far better answer than the
     * count: `open(mode: |)` is about whichever candidate declares a `mode`, whatever its arity and
     * whichever position it is in. Arity is the fallback, and the only thing there is to go on while
     * a positional call is being typed - the argument types are exactly what is not there yet.
     */
    auto written = writtenArgumentName(context, text, call.argumentStart, offset);
    U32 active = 0;

    for(U32 i = 0; i < candidates.size(); i++) {
        auto count = parameterCount(context, candidates[i]);
        auto fits = written ? parameterNamed(context, candidates[i], written) < count
                            : count > call.argument;

        if(fits) { active = i; break; }
    }

    /*
     * And the parameter that name reaches, rather than the one in its place - which is the whole of
     * what a named argument means at a call site. `subtract(take: |)` highlights `take`, wherever
     * `take` is declared, and falls back to the count where the name reaches nothing: a name being
     * typed is a prefix of one for as long as it takes to write, and jumping the highlight back to
     * the first parameter for every keystroke of it would be worse than leaving it where it was.
     */
    auto parameter = call.argument;

    if(written) {
        auto named = parameterNamed(context, candidates[active], written);
        if(named < parameterCount(context, candidates[active])) parameter = U32(named);
    }

    json.startObject();
    json.field("signatures"_v).startArray();
    for(auto& candidate: candidates) writeSignature(json, context, candidate);
    json.endArray();

    json.field("activeSignature"_v).value(active);
    json.field("activeParameter"_v).value(parameter);
    json.endObject();
}

/*
 * Semantic tokens.
 */

static StringView tokenTypeName(TokenType type) {
    switch(type) {
        case TokenType::Namespace: return "namespace"_v;
        case TokenType::Type: return "type"_v;
        case TokenType::Class: return "class"_v;
        case TokenType::TypeParameter: return "typeParameter"_v;
        case TokenType::Parameter: return "parameter"_v;
        case TokenType::Variable: return "variable"_v;
        case TokenType::Property: return "property"_v;
        case TokenType::EnumMember: return "enumMember"_v;
        case TokenType::Function: return "function"_v;
        case TokenType::Method: return "method"_v;
        default: return "variable"_v;
    }
}

void writeTokenLegend(Net::JsonWriter& json) {
    json.field("tokenTypes"_v).startArray();
    for(U32 i = 0; i < U32(TokenType::Count); i++) {
        json.arrayField().value(tokenTypeName(TokenType(i)));
    }
    json.endArray();

    json.field("tokenModifiers"_v).startArray();
    json.arrayField().value("declaration"_v);
    json.arrayField().value("definition"_v);
    json.arrayField().value("readonly"_v);
    json.arrayField().value("static"_v);
    json.arrayField().value("borrowed"_v);
    json.arrayField().value("sunk"_v);
    json.arrayField().value("heapPlaced"_v);
    json.endArray();
}

static TokenType tokenTypeOf(Symbol::Kind kind) {
    switch(kind) {
        case Symbol::Kind::Function: return TokenType::Function;

        // §11: a call that dispatches through an instance is not a direct call, and seeing which is
        // which without reading the declaration is the point of colouring them apart.
        case Symbol::Kind::ClassFun: return TokenType::Method;

        case Symbol::Kind::Global: return TokenType::Variable;
        case Symbol::Kind::Type:
        case Symbol::Kind::Alias: return TokenType::Type;

        // `Just` and `Maybe` are both a ConID to the lexer and mean different things.
        case Symbol::Kind::Constructor: return TokenType::EnumMember;

        case Symbol::Kind::Class: return TokenType::Class;
        case Symbol::Kind::Arg: return TokenType::Parameter;
        case Symbol::Kind::Local:
        case Symbol::Kind::Capture: return TokenType::Variable;
        case Symbol::Kind::Field: return TokenType::Property;
        case Symbol::Kind::TypeVar: return TokenType::TypeParameter;
        case Symbol::Kind::Module:
        case Symbol::Kind::Import: return TokenType::Namespace;
    }

    return TokenType::Variable;
}

// The ownership conventions and the storage decision, read off the slot the symbol names. This is
// the half of §11's table that is a *modifier*: it says something about the binding rather than
// replacing what the binding is.
static U32 tokenModifiersOf(const Symbol& symbol) {
    U32 modifiers = 0;

    switch(symbol.kind) {
        case Symbol::Kind::Global: {
            modifiers |= U32(TokenModifier::Static);
            if(symbol.module) {
                auto global_ = (*symbol.module->arena)[ModulePtr<Global>(symbol.payload)];
                if(!global_->mut) modifiers |= U32(TokenModifier::Readonly);
            }

            return modifiers;
        }
        case Symbol::Kind::Arg: {
            if(!symbol.module || !symbol.function) return modifiers;

            auto local = *symbol.module->arena;
            auto function = local[symbol.function];
            if(symbol.payload >= function->args.size()) return modifiers;

            auto arg = local[function->args.get(local, symbol.payload)];
            if(arg->convention == ast::BindType::Ref) modifiers |= U32(TokenModifier::Borrowed);
            else if(arg->convention == ast::BindType::Sink) modifiers |= U32(TokenModifier::Sunk);

            return modifiers;
        }
        case Symbol::Kind::Local: {
            if(!symbol.module || !symbol.function) return modifiers;
            if(symbol.payload == maxLimit<U32>) return modifiers;

            auto local = *symbol.module->arena;
            auto function = local[symbol.function];
            if(symbol.payload >= function->localCount()) return modifiers;

            auto slot = function->localAt(local, symbol.payload);
            if(slot.borrowed || slot.convention == ast::BindType::Ref) {
                modifiers |= U32(TokenModifier::Borrowed);
            } else if(slot.convention == ast::BindType::Sink) {
                modifiers |= U32(TokenModifier::Sunk);
            }

            // The `explain` cliff, inline. Off by default in any theme that has not asked for it.
            if(slot.storage == StorageClass::Heap) modifiers |= U32(TokenModifier::HeapPlaced);
            return modifiers;
        }
        case Symbol::Kind::Capture:
            return U32(TokenModifier::Borrowed);
        default:
            return modifiers;
    }
}

/*
 * Whether a range covers exactly one written name.
 *
 * Not every recorded reference does. A lookup made while resolving a call is recorded against the
 * *call*, because that is the location the resolver had when it asked - and colouring a whole call
 * as its callee would paint over every argument in it. So a token is emitted only for a range that
 * is one lexeme: no whitespace, no bracket, no comma. That is a property of the text rather than a
 * flag anything has to remember to set, which is what makes it hold for recording sites that do not
 * know this exists.
 */
static bool isOneLexeme(StringView text, U32 start, U32 end) {
    if(end <= start || end > text.length) return false;
    if(end - start > 256) return false;

    for(auto i = start; i < end; i++) {
        auto c = text.ptr[i];
        if(c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
        if(c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') return false;
        if(c == ',' || c == ';' || c == '"' || c == '\'') return false;
    }

    return true;
}

struct SemanticToken {
    U32 line = 0;
    U32 character = 0;
    U32 length = 0;
    U32 type = 0;
    U32 modifiers = 0;
    U32 start = 0;
    U32 end = 0;
};

// An insertion sort, for the reason PositionIndex::build uses one: the references are recorded in
// resolution order, which for one file is close enough to source order that this is a comparison
// per token and nothing else.
static void sortTokens(Array<SemanticToken>& tokens) {
    for(U32 i = 1; i < tokens.size(); i++) {
        auto token = tokens[i];
        auto j = i;
        while(j > 0 && token.start < tokens[j - 1].start) {
            tokens[j] = tokens[j - 1];
            j--;
        }

        tokens[j] = token;
    }
}

void writeSemanticTokens(Net::JsonWriter& json, Session& session, StringId module,
                         StringView text, const LineTable& lines, bool utf16) {
    Array<SemanticToken> tokens;

    if(session.index && session.context) {
        for(auto& reference: session.index->references) {
            auto location = session.context->getLocation(reference.source);
            if(!location || location->sourceModule != module) continue;

            auto start = location->sourceStart.offset;
            auto end = location->sourceEnd.offset;
            if(!isOneLexeme(text, start, end)) continue;

            SemanticToken token;
            token.start = start;
            token.end = end;
            token.line = lines.lineOf(start);
            token.character = utf16 ? lines.utf16Column(text, start) : start - lines.lineStart(token.line);
            token.length = utf16 ? lines.utf16Column(text, end) - token.character : end - start;
            token.type = U32(tokenTypeOf(reference.target.kind));
            token.modifiers = tokenModifiersOf(reference.target);

            // A token the protocol cannot express: the range crossed a line break, which
            // `isOneLexeme` already rules out for anything but a line ending inside a name.
            if(lines.lineOf(end) != token.line || token.length == 0) continue;

            tokens.push(token);
        }
    }

    sortTokens(tokens);

    json.startObject();
    json.field("data"_v).startArray();

    U32 previousLine = 0;
    U32 previousCharacter = 0;
    U32 previousEnd = 0;
    auto first = true;

    for(auto& token: tokens) {
        // The protocol has no way to say "these two overlap", and a client given a pair that does
        // draws whichever it read last over the other. Two records at one position is a legitimate
        // state - a name looked up twice - so the later one is dropped rather than reported.
        if(!first && token.start < previousEnd) continue;

        auto deltaLine = token.line - previousLine;
        auto deltaCharacter = deltaLine ? token.character : token.character - previousCharacter;

        json.arrayField().value(deltaLine);
        json.arrayField().value(deltaCharacter);
        json.arrayField().value(token.length);
        json.arrayField().value(token.type);
        json.arrayField().value(token.modifiers);

        previousLine = token.line;
        previousCharacter = token.character;
        previousEnd = token.end;
        first = false;
    }

    json.endArray();
    json.endObject();
}

} // namespace lsp
