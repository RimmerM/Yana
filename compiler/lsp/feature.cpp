#include "feature.h"

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
        json.arrayField();
        if(!locations.writeLocation(json, occurrence->source)) json.null();
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
