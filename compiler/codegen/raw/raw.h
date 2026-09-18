#pragma once

#include "../../compiler/context.h"

/*
 * The raw executable format is deliberately only a byte writer. Layout, entry placement and
 * relocation are the code generator's job; a flat image has no header in which to record any of
 * those facts. In particular, this writer must not grow a boot stub or assume an operating system.
 */

// Writes exactly `size` bytes from `bytes` to `path`, reporting filesystem failures through the
// compiler diagnostics. The caller has already resolved every address against the selected image
// base and decides which byte is the entry point.
bool writeRawImage(Context& context, const Byte* bytes, Size size, const String& path);
