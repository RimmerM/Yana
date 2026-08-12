#pragma once

#include "../compiler/compiler/settings.h"

/*
 * Codegen settings a fixture selects for itself, written as a comment on any line of its source:
 *
 *     # extensions: avx2
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
 * `explicitExtensions` is set for the reason the command line sets it: without it the backend reads
 * the *host's* CPUID, and a golden that changed with the machine that ran it would assert nothing.
 * A fixture with no directive is therefore compiled for the baseline on every machine.
 */
inline void applyExtensionDirective(CompileSettings& settings, StringView content) {
    // Longest first, so that "avx512" is not read as "avx" with trailing text and "sse4.2" not as
    // "sse4.1" - the two share every character up to the last.
    static const struct { StringView name; TargetExtensions::SSEMode sse; } levels[] = {
        { "avx512"_v, TargetExtensions::AVX512 },
        { "avx2"_v, TargetExtensions::AVX2 },
        { "avx"_v, TargetExtensions::AVX },
        { "sse4.2"_v, TargetExtensions::SSE4_2 },
        { "sse4.1"_v, TargetExtensions::SSE4_1 },
    };

    auto directive = "# extensions: "_v;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        auto rest = content.ptr + i + directive.length;
        auto left = content.length - i - directive.length;

        for(auto& level: levels) {
            if(level.name.length > left) continue;
            if(compareMem(rest, level.name.ptr, level.name.length) != 0) continue;

            settings.extensions.sse = level.sse;
            settings.explicitExtensions = true;

            // The implication the command line draws too: every part with AVX2 has FMA3 and LZCNT,
            // and those are flags beside the level rather than points on it. A fixture naming a
            // level would otherwise get a target that can encode `vfmadd` and does not claim to,
            // which is the one form in the table whose feature is not a level.
            if(level.sse >= TargetExtensions::AVX2) {
                settings.extensions.fma3 = true;
                settings.extensions.lzcnt = true;
            }

            return;
        }
    }
}
