#pragma once

#include "../compiler/compiler/settings.h"

/*
 * Settings a fixture selects for itself, written as a comment on any line of its source:
 *
 *     # extensions: avx2
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
 * **A fixture with no directive is compiled for `sse4`**, which is the floor and the struct's own
 * default - not the host's level, which is what the command line takes when it is told nothing. A
 * golden that changed with the machine that ran it would assert nothing, so no driver here ever
 * calls `applyDefaults` and no driver here ever detects anything.
 */
inline void applyExtensionDirective(CompileSettings& settings, StringView content) {
    auto directive = "# extensions: "_v;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        auto rest = content.ptr + i + directive.length;
        auto left = content.length - i - directive.length;

        /*
         * **Every name on the line, not just the first.** `# extensions: sha avx2` is two facts about
         * one target and the two are independent - a level says which instructions the compiler may
         * choose and `sha` says whether an extension beside it is claimed, exactly as they are on the
         * command line where `-enable-inst sha` is a second flag rather than a replacement for the
         * first. Reading only the leading token made the second name a comment: `sha v3` claimed the
         * extension and quietly compiled for the floor.
         *
         * The scan stops at the end of the line, so a directive says nothing about the prose under
         * it. Several `# extensions:` lines in one file still accumulate, which is what the outer
         * loop is for.
         *
         * **A fixture that names one will fault on a machine without it**, which is
         * `CopyMemory.Avx2.yana`'s bargain: these drivers execute what they compile, so a fixture
         * asking for an instruction set is asking for one the machine running it has. Nothing here
         * detects; a fixture that names nothing is compiled for `sse4` without any extension, which is
         * what keeps the rest of the corpus reproducible.
         */
        for(Size at = 0; at < left && rest[at] != '\n' && rest[at] != '\r';) {
            if(rest[at] == ' ' || rest[at] == '\t' || rest[at] == ',') { at++; continue; }

            // One word, read to the next separator, and then asked of the tables the command line
            // reads - so a directive is spelled exactly the way `-enable-inst` is, and a spelling the
            // compiler refuses (`v3`) is one a fixture cannot use either.
            auto end = at;
            while(end < left && rest[end] != ' ' && rest[end] != '\t' && rest[end] != ','
                  && rest[end] != '\n' && rest[end] != '\r') end++;

            auto word = StringView { rest + at, end - at };
            auto matched = Size(0);

            if(auto level = matchLevel(word)) {
                // The highest of everything named, as `-enable-inst` takes it: two names on one line
                // are two facts about one target, and the second must not undo the first.
                if(settings.extensions.level < level.unwrap()) settings.extensions.level = level.unwrap();
                settings.explicitExtensions = true;
                matched = word.length;
            } else if(auto flag = matchExtensionFlag(word)) {
                settings.extensions.flags |= flag;
                settings.explicitExtensions = true;
                matched = word.length;
            }

            // A spelling the compiler used to take and now refuses is a fixture compiled for the
            // floor behind its author's back, which is the one failure here that would be silent -
            // so it stops the driver rather than ending the line.
            if(!matched && replacedLevelSpelling(word)) {
                println("A fixture names the instruction set level `%@`, which is spelled `%@` now.",
                        word, replacedLevelSpelling(word).unwrap());
                abort();
            }

            // An unrecognized word ends the line's meaning rather than being skipped over, so a typo
            // is a fixture that does not get what it asked for rather than one that silently gets
            // half of it - and the fixture's own assertions are what report it.
            if(!matched) break;
            at += matched;
        }
    }

    // And the one check the command line makes of the pair - a flag below the lowest level it has an
    // encoding at, which is `-enable-inst`'s refusal (see checkExtensionFlags). A fixture that asked
    // for `vaes` at `sse4` would otherwise be compiled with a set the compiler refuses to be told.
    if(auto refused = checkExtensionFlags(settings.extensions)) {
        println("%@", refused.unwrap());
        abort();
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
 * `# checks: located`, which is `-check-locations` - CompileSettings::checkLocations.
 *
 * Beside `off` rather than in a directive of its own, because the two are the same question asked
 * three ways: a check that is not there, one that stops with a status, and one that says where it
 * was. Only the third direction needs saying - a fixture that wrote `located` and one that wrote
 * nothing would otherwise be the same fixture.
 */
inline void applyCheckLocationDirective(CompileSettings& settings, StringView content) {
    auto directive = "# checks: located"_v;
    if(content.length < directive.length) return;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        settings.checkLocations = true;
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

/*
 * `# resolver: ownership` - CompileSettings::resolver, and the one directive that is about which
 * *compiler* reads the fixture rather than what it compiles for.
 *
 * A fixture saying this is one the rewritten stage in `compiler/resolve_mir` can compile and the
 * shipping stage cannot, which is a real category while the rewrite is under way: the new stage
 * already accepts programs the old one rejects - a partial move out of a record is the first of
 * them - and there is nowhere to write such a program down otherwise. `test/resolve/ownership/` is
 * where they live and this is what makes them run, which are two different statements: the
 * directory is filing, and a fixture anywhere carrying this line is compiled the same way.
 *
 * What it does *not* do is opt a fixture into the new stage. That is the `.mir.expect` file beside
 * the source and is how the shared corpus is adopted one fixture at a time - a fixture with the
 * golden and no directive is compiled by *both* stages and asserts that they agree, which is the
 * most valuable thing either of them can say. This directive is only for the programs where that
 * comparison is not available, and it costs the comparison, so it is worth writing only when the
 * old stage genuinely cannot compile the program.
 *
 * Only the one direction, and the direction is what this pins: **a fixture's stage is the corpus's
 * to state, not the command line's**. `CompileSettings::resolver` defaults to the rewrite since
 * 2026-09-22 (J81), and every golden in this corpus that is not the rewrite's own was written by
 * `compiler/resolve` - so the baseline here is `Legacy` whatever the compiler's default is, and this
 * directive is the only thing that raises a fixture out of it. The alternative, letting the drivers
 * inherit the flag's default, is a flip of that default silently reclassifying nine hundred goldens
 * and the disagreement oracle asserting that the rewrite agrees with itself. The passes that *are*
 * the rewrite's set `ResolverStage::Ownership` after this runs - see `ownershipSettings`.
 */
inline void applyResolverDirective(CompileSettings& settings, StringView content) {
    settings.resolver = ResolverStage::Legacy;

    auto directive = "# resolver: ownership"_v;
    if(content.length < directive.length) return;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        settings.resolver = ResolverStage::Ownership;
        return;
    }
}

/*
 * `# test: yes` - `-test`, and Design-Test.md §11.2's F1.
 *
 * A fixture saying this has `@test` declarations and no `main`, so what runs it is the entry the
 * compiler synthesizes out of them. It needs a `Test` module to build that entry from, which is
 * `resolve/modules/Test.yana` - a minimal runner both resolve stages compile, so that the harness
 * itself is a thing the shared corpus can assert rather than only the library suite.
 *
 * Only the one direction, like `# checks: off`: a build without `-test` is the default and a fixture
 * saying so would be saying nothing. It is also the one directive that changes what the *entry* is,
 * which is why a fixture carrying it has no `main` at all.
 */
inline void applyTestDirective(CompileSettings& settings, StringView content) {
    auto directive = "# test: yes"_v;
    if(content.length < directive.length) return;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        settings.test = true;
        return;
    }
}

/*
 * `# allocator: <name>` - `-allocator`, and Design-Allocator.md §18.3's provider selection.
 *
 * The one directive whose axis is *policy* rather than machine or compiler, and the reason a fixture
 * wants it is that which provider a build has changes the **program**, not only its speed. A
 * provider that answers `isHeapAddress` lets the compiler release a boxed field by asking the
 * address where it came from; one that does not makes every record with a box carry a bit saying so.
 * The two are different layouts and different code, and `Box.Classified.yana` and `Box.Stored.yana`
 * are one program compiled under each.
 *
 * The name is opaque - `CompileSettings::allocator` and `allocatorSelector` say why the compiler
 * knows no roster - so anything up to the end of the line is the provider, trimmed. A fixture with
 * no directive gets `bootstrap`, which is the struct's own default and what the command line starts
 * at.
 */
inline void applyAllocatorDirective(CompileSettings& settings, StringView content) {
    auto directive = "# allocator: "_v;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        auto rest = content.ptr + i + directive.length;
        auto left = content.length - i - directive.length;

        Size end = 0;
        while(end < left && rest[end] != '\n' && rest[end] != '\r' && rest[end] != ' ' &&
              rest[end] != '\t') {
            end++;
        }

        // An empty name selects nothing rather than selecting "": a directive with no provider after
        // it is a typo, and leaving the default in place is what makes the fixture's own assertions
        // report it.
        if(!end) return;

        settings.allocator = Tritium::String(rest, end);
        return;
    }
}

// Every directive at once. The drivers call this rather than any one of them, which is what keeps
// the three of them reading the same fixture the same way - see the note above about why that is not
// merely tidiness.
/*
 * `# platform: js` - `CompileSettings::platform`, and the second directive about *what* a fixture is
 * rather than about how it is compiled.
 *
 * A fixture saying this is a JavaScript program: `@platform` selection reads `js`, the file-name
 * selectors pick `Array.js.yana` over `Array.native.yana`, and nothing about it may be run on a
 * machine. It exists for the one thing that has no other spelling - a program whose *diagnostic* is
 * about the host, `Reject.HostMember.yana` being the first - because a host role is declared only in
 * `lib/Native/Host.js.yana` and a native build cannot reach one to be refused.
 *
 * Only the one direction, like every directive here: `native` is the default and a fixture saying so
 * would be saying nothing. A fixture that carries it must also carry `# resolver: ownership` or have
 * a `.js.ssa.expect` beside it, because those are the two paths in `ResolveTester` that read it;
 * there is deliberately no check for that, on the grounds that the failure is the fixture reporting
 * against the wrong library rather than anything silent.
 */
inline void applyPlatformDirective(CompileSettings& settings, StringView content) {
    auto directive = "# platform: js"_v;
    if(content.length < directive.length) return;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        settings.platform = TargetPlatform::Js;
        return;
    }
}

/*
 * `# target: bare` - `-target bare`, the freestanding operating system: no kernel, so the library's
 * platform layer is `lib/Native/Bare.bare.x64.yana` rather than `Linux.linux.x64.yana`, and a `main`
 * may take parameters because the loader enters it directly.
 *
 * The third directive about *what* a fixture is, and applied the way the command line applies the
 * flag - `settleTargetAndFormat` with a target named and no format - so a fixture and `yana
 * -target bare` build the same thing. Only the one value: every other operating system is either
 * the host's, which is the default, or one this machine cannot run, and a bare fixture is one no
 * driver runs either - it is asserted by its dumps and its diagnostics. `# format:` is not a
 * directive, because nothing a fixture asserts reads the executable format.
 */
inline void applyTargetDirective(CompileSettings& settings, StringView content) {
    auto directive = "# target: bare"_v;
    if(content.length < directive.length) return;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        settings.target = TargetType::Bare;
        settleTargetAndFormat(settings, true, false);
        return;
    }
}

/*
 * `# recover: yes` - `CompileSettings::recover`, which the driver turns on for every build through
 * the rewritten stage and a fixture does not get by default.
 *
 * Off by default because every rejection fixture's golden was written against the stage stopping at
 * its first failing checkpoint, and what those fixtures assert is *a* diagnostic rather than how far
 * past it a compile gets. A fixture about that - `resolve/Recover.*.yana` - asks for it by name.
 */
inline void applyRecoverDirective(CompileSettings& settings, StringView content) {
    auto directive = "# recover: yes"_v;
    if(content.length < directive.length) return;

    for(Size i = 0; i + directive.length <= content.length; i++) {
        if(compareMem(content.ptr + i, directive.ptr, directive.length) != 0) continue;

        settings.recover = true;
        return;
    }
}

inline void applyFixtureDirectives(CompileSettings& settings, StringView content) {
    applyExtensionDirective(settings, content);
    applyPlatformDirective(settings, content);
    applyTargetDirective(settings, content);
    applyChecksDirective(settings, content);
    applyCheckLocationDirective(settings, content);
    applyInliningDirective(settings, content);
    applyResolverDirective(settings, content);
    applyTestDirective(settings, content);
    applyRecoverDirective(settings, content);
    applyAllocatorDirective(settings, content);
}

/*
 * Whether a fixture is one only the rewritten stage compiles, asked without a `CompileSettings`.
 *
 * The drivers that do not have one yet at the point they decide what to run - the ELF suite listing
 * its corpus, the benchmark collecting its fixtures - ask this, so that "what does this line mean"
 * is answered by `applyResolverDirective` above and by nothing else. Those drivers skip such a
 * fixture entirely: they compile with the shipping stage, and the fixture says that will not work.
 */
inline bool fixtureNeedsOwnershipStage(StringView content) {
    CompileSettings settings;
    applyResolverDirective(settings, content);
    return settings.resolver == ResolverStage::Ownership;
}

/*
 * And whether a fixture is a **JavaScript** program, asked the same way and for the same readers.
 *
 * A fixture carrying `# platform: js` is compiled for that target and nothing else: it is not run,
 * it has no native goldens, and every driver that compiles for a machine skips it. Such a fixture
 * always carries `# resolver: ownership` as well - the shipping stage compiles JavaScript too, but
 * what these fixtures are *about* is the rewrite's answer for it - so the drivers that already skip
 * on the line above skip these without a second test.
 */
inline bool fixtureIsJavaScript(StringView content) {
    CompileSettings settings;
    applyPlatformDirective(settings, content);
    return settings.platform == TargetPlatform::Js;
}
