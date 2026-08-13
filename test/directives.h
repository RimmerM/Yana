#pragma once

#include "../compiler/compiler/settings.h"

/*
 * Codegen settings a fixture selects for itself, written as a comment on any line of its source:
 *
 *     # extensions: v3
 *
 * Read by three drivers - the resolve suite, the ELF suite and the x64 byte listings - which is why
 * it is here rather than in any one of them. **They have to agree**, and not merely for tidiness:
 * `YanaResolveTest` and `YanaElfTest` compile the same `resolve/*.yana` corpus, so a fixture whose
 * level one of them reads and the other does not is one compiled at two different vector widths -
 * and since `targetVectorBytes` decides what `Vec(Float)` *is*, that is not a difference of
 * optimization but a difference of program. The first version of this lived in one driver and the
 * other rejected `lane(v, 5)` as past the end of a four-lane vector.
 *
 * A `#` is not a comment in Yana, so a `.yana` fixture writes the directive inside its own block
 * comment; a `.lower` fixture writes it at the top, where `#` is the comment character. The scan is
 * a plain substring search either way, which is what lets one reader serve both.
 *
 * **A fixture with no directive is compiled for v2**, which is the floor and the struct's own
 * default - not the host's level, which is what the command line takes when it is told nothing. A
 * golden that changed with the machine that ran it would assert nothing, so no driver here ever
 * calls `applyDefaults` and no driver here ever detects anything.
 */
inline void applyExtensionDirective(CompileSettings& settings, StringView content) {
    // Longest first, so that "avx512" is not read as "avx" with trailing text - the two share every
    // character up to the last.
    static const struct { StringView name; TargetExtensions::Level level; } levels[] = {
        { "avx512"_v, TargetExtensions::V4 },
        { "avx2"_v, TargetExtensions::V3 },
        { "v4"_v, TargetExtensions::V4 },
        { "v3"_v, TargetExtensions::V3 },
        { "v2"_v, TargetExtensions::V2 },
        { "sse4.2"_v, TargetExtensions::V2 },
    };

    auto directive = "# extensions: "_v;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        auto rest = content.ptr + i + directive.length;
        auto left = content.length - i - directive.length;

        for(auto& level: levels) {
            if(level.name.length > left) continue;
            if(compareMem(rest, level.name.ptr, level.name.length) != 0) continue;

            settings.extensions.level = level.level;
            settings.explicitExtensions = true;
            return;
        }
    }
}
