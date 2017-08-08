#include "string.h"

// UTF-32 --> UTF-8 conversion.
char* encodeCodePoint(U32 codePoint, char* buffer) {
    if(codePoint <= 0x7F) {
        // One character.
        buffer[0] = (char)codePoint;
        return buffer + 1;
    } else if(codePoint <= 0x07FF) {
        // Two characters.
        buffer[0] = (char)(0xC0 | (codePoint >> 6));
        buffer[1] = (char)(0x80 | (codePoint & 0x3F));
        return buffer + 2;
    } else if(codePoint <= 0xFFFF) {
        // Three characters.
        buffer[0] = (char)(0xE0 | (codePoint >> 12));
        buffer[1] = (char)(0x80 | ((codePoint >> 6) & 0x3F));
        buffer[2] = (char)(0x80 | (codePoint & 0x3F));
        return buffer + 3;
    } else if(codePoint <= 0x10FFFF) {
        // Four characters.
        buffer[0] = (char)(0xF0 | (codePoint >> 18));
        buffer[1] = (char)(0x80 | ((codePoint >> 12) & 0x3F));
        buffer[2] = (char)(0x80 | ((codePoint >> 6) & 0x3F));
        buffer[3] = (char)(0x80 | (codePoint & 0x3F));
        return buffer + 4;
    } else {
        // Invalid code point.
        *buffer = 0;
        return buffer + 1;
    }
}

// UTF-8 --> UTF-32 conversion.
U32* convertNextPoint(const char*& string, U32* dest) {
    // Get the bytes from the first Char.
    U32 p;
    auto s = (Byte*)string;
    U32 c = s[0];
    string++;

    if(c < 0x80) {
        // 1-byte sequence, 00->7F
        *dest = (U32)c;
        return dest + 1;
    }

    if(c < 0xC2) {
        // Invalid sequence, 80->C1
        goto fail;
    }

    if(c < 0xE0) {
        // 2-byte sequence, C0->DF
        p = (c & 0x1F);
        goto _2bytes;
    }

    if(c < 0xF0) {
        // 3-byte sequence, E0->EF
        p = (c & 0xF);
        goto _3bytes;
    }

    if(c < 0xF8) {
        // 4-byte sequence, F0->F7
        p = (c & 0x7);

        // Valid Unicode cannot be higher than 0x10FFFF.
        if(p > 8) goto fail;
        else goto _4bytes;
    }

    fail:
    string = nullptr;
    return nullptr;

    _4bytes:
    {
        string++;
        s++;
        U32 fourth = *s;

        //Fourth Char must be 10xxxxxx
        if((fourth >> 6) != 2) goto fail;

        p <<= 6;
        p |= (fourth & 0x3F);
    }
    _3bytes:
    {
        string++;
        s++;
        U32 third = *s;

        //Third Char must be 10xxxxxx
        if((third >> 6) != 2) goto fail;

        p <<= 6;
        p |= (third & 0x3F);
    }
    _2bytes:
    string++;
    s++;
    U32 second = *s;

    //Second Char must be 10xxxxxx
    if((second >> 6) != 2) goto fail;

    p <<= 6;
    p |= (second & 0x3F);
    *dest = p;
    return dest + 1;
}