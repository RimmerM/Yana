#pragma once

#include <Core.h>
#include <Mem/ChunkedBuffer.h>

namespace lsp {

using namespace Tritium;

/*
 * A decoded JSON value.
 *
 * Tritium's `JsonToken` is a pull tokenizer, which is the right shape for a protocol whose messages
 * have one known form. LSP's do not: a request is dispatched on a `method` field that may arrive
 * after the `params` it decides the meaning of, and half the fields in the specification are
 * optional. So the tokenizer builds a tree once and the handlers read it by name, rather than every
 * handler being a state machine over field order.
 *
 * Children are a linked list rather than an array because a message is read once and thrown away:
 * the list costs one pointer per value and no reallocation, and nothing here is ever indexed in a
 * loop that would notice the difference.
 */
struct JsonValue {
    enum Kind: U8 {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Null;
    bool boolean = false;
    double number = 0;

    /// The text of a string, or - for a member of an object - the name it was stored under.
    StringView string;
    StringView name;

    JsonValue* first = nullptr;
    JsonValue* last = nullptr;
    JsonValue* next = nullptr;

    /// The member of an object with this name, or null. Null-safe on the receiver, so that a chain
    /// of lookups through fields that may not exist reads as one expression.
    const JsonValue* find(StringView name) const;
    const JsonValue* at(U32 index) const;
    U32 count() const;

    bool isNull() const { return kind == Null; }

    StringView asString(StringView fallback = {}) const;
    double asNumber(double fallback = 0) const;
    I64 asInt(I64 fallback = 0) const;
    bool asBool(bool fallback = false) const;
};

/// A lookup that tolerates a missing parent, so `find(message, "params")->find("textDocument")`
/// does not have to be written as three checks.
const JsonValue* find(const JsonValue* value, StringView name);

/*
 * One decoded message, and the storage everything in it points into.
 *
 * The values and the text of every string live in `storage`, so a message is freed by freeing one
 * object and no part of it may outlive that.
 */
struct JsonDocument {
    JsonDocument(): storage(4096) {}
    JsonDocument(JsonDocument&&) = default;

    /// Decodes one complete JSON text. Returns false and sets `error` on malformed input - which is
    /// a protocol error rather than an assertion, because the bytes came from another process.
    bool parse(Buffer<Byte> text);

    ChunkedBuffer storage;
    JsonValue* root = nullptr;
    String error;
};

} // namespace lsp
