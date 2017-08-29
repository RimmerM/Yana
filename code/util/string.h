#pragma once

#include "types.h"

// UTF-32 --> UTF-8 conversion.
char* encodeCodePoint(U32 codePoint, char* buffer);

// UTF-8 --> UTF-32 conversion.
U32* convertNextPoint(const char*& string, U32* dest);

char* copyString(const char* string, char* buffer, U32 bufferSize);