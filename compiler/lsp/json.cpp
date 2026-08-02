#include "json.h"
#include "Net/Codec/Json.h"

namespace lsp {

using Tritium::Net::JsonToken;

const JsonValue* JsonValue::find(StringView name) const {
    if(kind != Object) return nullptr;

    for(auto child = first; child; child = child->next) {
        if(child->name.length == name.length
        && compareMem(child->name.ptr, name.ptr, name.length) == 0) {
            return child;
        }
    }

    return nullptr;
}

const JsonValue* JsonValue::at(U32 index) const {
    if(kind != Array) return nullptr;

    U32 i = 0;
    for(auto child = first; child; child = child->next) {
        if(i == index) return child;
        i++;
    }

    return nullptr;
}

U32 JsonValue::count() const {
    U32 i = 0;
    for(auto child = first; child; child = child->next) i++;
    return i;
}

StringView JsonValue::asString(StringView fallback) const {
    return kind == String ? string : fallback;
}

double JsonValue::asNumber(double fallback) const {
    return kind == Number ? number : fallback;
}

I64 JsonValue::asInt(I64 fallback) const {
    return kind == Number ? I64(number) : fallback;
}

bool JsonValue::asBool(bool fallback) const {
    return kind == Bool ? boolean : fallback;
}

const JsonValue* find(const JsonValue* value, StringView name) {
    return value ? value->find(name) : nullptr;
}

// Builds one value from the token stream. `parse()` has to be called before the kind is known, so
// the end markers arrive here as values and are answered with null - which is how the object and
// array loops below find their end without peeking.
static JsonValue* parseValue(JsonToken& token, ChunkedBuffer& storage) {
    token.parse();

    if(token.kind == JsonToken::EndObject || token.kind == JsonToken::EndArray) return nullptr;

    auto value = new(storage) JsonValue();

    switch(token.kind) {
        case JsonToken::Null:
            value->kind = JsonValue::Null;
            break;
        case JsonToken::Bool:
            value->kind = JsonValue::Bool;
            value->boolean = token.boolPayload;
            break;
        case JsonToken::Number:
            value->kind = JsonValue::Number;
            value->number = token.numberPayload;
            break;
        case JsonToken::String:
            value->kind = JsonValue::String;
            // The tokenizer allocated this out of the same storage, so it lives exactly as long as
            // the value pointing at it does and does not have to be copied.
            value->string = token.stringPayload;
            break;
        case JsonToken::Array: {
            value->kind = JsonValue::Array;
            while(auto element = parseValue(token, storage)) {
                if(value->last) value->last->next = element; else value->first = element;
                value->last = element;
            }
            break;
        }
        case JsonToken::Object: {
            value->kind = JsonValue::Object;
            while(true) {
                token.parse();
                if(token.kind == JsonToken::EndObject) break;
                if(token.kind != JsonToken::FieldName) throw Net::JsonException("expected a field name");

                auto name = token.stringPayload;
                auto member = parseValue(token, storage);
                if(!member) throw Net::JsonException("expected a field value");

                member->name = name;
                if(value->last) value->last->next = member; else value->first = member;
                value->last = member;
            }
            break;
        }
        case JsonToken::FieldName:
        default:
            throw Net::JsonException("unexpected json token");
    }

    return value;
}

bool JsonDocument::parse(Buffer<Byte> text) {
    root = nullptr;
    error = String();

    try {
        Net::Reader reader(text);
        JsonToken token(reader, storage);
        root = parseValue(token, storage);
        if(!root) error = String("empty json message");
    } catch(const Net::Exception& e) {
        error = e.description;
        root = nullptr;
    }

    return root != nullptr;
}

} // namespace lsp
