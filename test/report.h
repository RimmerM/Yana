#pragma once

#include <Core.h>

/*
 * A test binary's report stream, as far as a driver needs it - Design-Test.md §5.2.
 *
 * `Test.runMain` writes one line per event to descriptor 3 when something has opened it:
 *
 *     begin 17 Map.duplicate_insert
 *     fail  17 Map.test:41:5  expected 1, got 2
 *     end   17 fail 214000
 *
 * Line-oriented and textual, and that is the property a driver is here for: a case that takes the
 * process down loses one line rather than the whole report, so a `begin` with no `end` still names
 * the case that died. `test/README.md` describes the opposite as what this used to look like - "the
 * log ends after a passing fixture and the process is simply gone" - and closing that is the whole
 * of what reading this buys.
 *
 * Shared by `YanaLibTest`, which forks and JITs, and `YanaElfTest`, which spawns a real executable.
 * The two put the descriptor in place differently and read the same lines out of it.
 */
struct TestReport {
    /// The case that was open when the stream stopped, and empty for a run that finished its report.
    Tritium::String unfinished;

    /// How many cases began, and how many claims did not hold. The claims are the count a report
    /// says out loud; the cases are what makes an empty run distinguishable from a passing one.
    int ran = 0;
    int failedClaims = 0;

    /// The `fail` lines themselves, indented, ready to print under a driver's own verdict. They
    /// carry the source location and the values, which nothing else in a driver has.
    Tritium::String failures;
};

/// Anything not recognized is passed over rather than dropped into an error: a fixture is free to
/// write to the same stream, and a reader that refused what it did not understand would hide
/// exactly the sentence somebody added for a human.
inline void readTestReport(const Tritium::String& text, TestReport& report) {
    using namespace Tritium;
    Size start = 0;

    for(Size i = 0; i <= text.size(); i++) {
        if(i != text.size() && text.text()[i] != '\n') continue;

        StringView line { text.text() + start, i - start };
        start = i + 1;

        if(line.startsWith("begin "_v)) {
            // `begin <ordinal> <name>`, of which the name is everything after the second space.
            Size at = 6;
            while(at < line.length && line.ptr[at] != ' ') at++;
            while(at < line.length && line.ptr[at] == ' ') at++;

            report.unfinished = ownedString(StringView { line.ptr + at, line.length - at });
            report.ran++;
        } else if(line.startsWith("fail  "_v)) {
            report.failedClaims++;
            report.failures = report.failures + String("  ") + ownedString(line) + String("\n");
        } else if(line.startsWith("end   "_v)) {
            report.unfinished = String();
        }
    }
}
