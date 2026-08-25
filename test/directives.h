#pragma once

#include "../compiler/compiler/settings.h"

/*
 * Settings a fixture selects for itself, written as a comment on any line of its source:
 *
 *     # extensions: v3
 *     # checks: off
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
 * `# checks: off` is `-no-checks` - see CompileSettings::checks. One fixture wants it and the reason
 * is worth stating, because "turn the checks off" reads like a performance switch: integer division
 * by zero has a *defined answer* in this language, and the check that reports it is what stands
 * between a fixture and observing that answer. `Divide.yana` asserts the answer, so it is compiled
 * the way a program that has decided to live with it would be. The rest of the rule - the pair the
 * machine would trap on - needs no directive and is asserted in the checked build beside it.
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

        /*
         * **Every name on the line, not just the first.** `# extensions: sha v3` is two facts about
         * one target and the two are independent - a level says which instructions the compiler may
         * choose and `sha` says whether an extension beside it is claimed, exactly as they are on the
         * command line where `-enable-inst sha` is a second flag rather than a replacement for the
         * first. Reading only the leading token made the second name a comment: `sha v3` claimed the
         * extension and quietly compiled for v2.
         *
         * The scan stops at the end of the line, so a directive says nothing about the prose under
         * it. Several `# extensions:` lines in one file still accumulate, which is what the outer
         * loop is for.
         *
         * **A fixture that names one will fault on a machine without it**, which is
         * `CopyMemory.Avx2.yana`'s bargain: these drivers execute what they compile, so a fixture
         * asking for an instruction set is asking for one the machine running it has. Nothing here
         * detects; a fixture that names nothing is compiled for v2 without any extension, which is
         * what keeps the rest of the corpus reproducible.
         */
        for(Size at = 0; at < left && rest[at] != '\n' && rest[at] != '\r';) {
            if(rest[at] == ' ' || rest[at] == '\t' || rest[at] == ',') { at++; continue; }

            auto remaining = left - at;
            auto matched = Size(0);

            for(auto& level: levels) {
                if(level.name.length > remaining) continue;
                if(compareMem(rest + at, level.name.ptr, level.name.length) != 0) continue;

                settings.extensions.level = level.level;
                settings.explicitExtensions = true;
                matched = level.name.length;
                break;
            }

            if(!matched && "sha"_v.length <= remaining && compareMem(rest + at, "sha"_v.ptr, 3) == 0) {
                settings.extensions.sha = true;
                settings.explicitExtensions = true;
                matched = "sha"_v.length;
            }

            // An unrecognized word ends the line's meaning rather than being skipped over, so a typo
            // is a fixture that does not get what it asked for rather than one that silently gets
            // half of it - and the fixture's own assertions are what report it.
            if(!matched) break;
            at += matched;
        }
    }
}

// `# checks: off`, which is the one directive that is not about the machine. Deliberately only the
// one direction: on is the default and a fixture that says so would be saying nothing.
inline void applyChecksDirective(CompileSettings& settings, StringView content) {
    auto directive = "# checks: off"_v;
    if(content.length < directive.length) return;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        settings.checks = false;
        return;
    }
}

/*
 * `# inline: none|size|balanced|speed` - CompileSettings::inlining.
 *
 * The size-against-speed knob, and the only one this compiler has. Two things read it and a fixture
 * may be about either: how far `compiler/opt` inlines, and how far the amd64 backend straight-lines
 * a block operation with a compile-time size (see BlockExpansion in codegen/x64/target.h). The
 * second is what makes it worth a directive, because the ceiling it sets is a *number* rather than
 * a heuristic - a fixture naming a level pins exactly how many transfers a copy of a given size
 * comes to, which is not a thing a golden compiled at the default can state.
 */
inline void applyInliningDirective(CompileSettings& settings, StringView content) {
    static const struct { StringView name; InlineLevel level; } levels[] = {
        { "balanced"_v, InlineLevel::Balanced },
        { "speed"_v, InlineLevel::Speed },
        { "none"_v, InlineLevel::None },
        { "size"_v, InlineLevel::Size },
    };

    auto directive = "# inline: "_v;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        auto rest = content.ptr + i + directive.length;
        auto left = content.length - i - directive.length;

        for(auto& level: levels) {
            if(level.name.length > left) continue;
            if(compareMem(rest, level.name.ptr, level.name.length) != 0) continue;

            settings.inlining = level.level;
            return;
        }
    }
}

// Every directive at once. The drivers call this rather than any one of them, which is what keeps
// the three of them reading the same fixture the same way - see the note above about why that is not
// merely tidiness.
inline void applyFixtureDirectives(CompileSettings& settings, StringView content) {
    applyExtensionDirective(settings, content);
    applyChecksDirective(settings, content);
    applyInliningDirective(settings, content);
}
