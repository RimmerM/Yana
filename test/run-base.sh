#!/usr/bin/env bash
#
# The standard library's own test suite - Design-Test.md §3.1 and §13.
#
# `lib/**/*.test.yana` are files of the modules they test, so there is no fixture list here: the
# suite is one program, `test/base/Base.yana`, which imports every module of the package. What makes
# those test files visible at all is `-package base` against `lib/yana.toml` - a consumer of the
# library never sees them, which is the whole point of the boundary.
#
# **Nothing here renders.** The runner draws its own report (lib/Test/Console.yana), and this script
# is only the loop that builds each row of the matrix and reports which row failed. That is the same
# division the report stream already draws: a parent that reads a child's stream prints for the
# human, and here the child *is* what a human reads.
set -u

cd "$(dirname "$0")"
build="${1:-../build-release}"
yana="$build/compiler/Yana"
shift || true

if [ ! -x "$yana" ]; then
    echo "no compiler at $yana - build it first, or name another build directory" >&2
    exit 1
fi

out="${TMPDIR:-/tmp}/yana-base-suite"
rm -rf "$out"
mkdir -p "$out"

# §8's matrix: a library's answer may not depend on how it was compiled. Each row is one build of
# the same sources, and every one of them runs the same cases.
#
# `generic` is why `-specialize` exists: it was reachable only from inside the C++ driver, which is
# what kept this suite from being an ordinary program.
run_row() {
    local name="$1"; shift
    local dir="$out/$name"

    # **The row is named before the build and not after it**, which is not cosmetic: the compile is
    # ~1.7s and the run it produces is ~70ms, so a script that named the row only once it had
    # something to report spent almost all of its wall time printing nothing at all - and looked,
    # from outside, exactly like a run that had hung. What a reader took for a missing progress bar
    # was this: the bar is drawn, for the seventy milliseconds it has.
    echo "== $name: building"

    # **v2, explicitly.** The command line takes the *host's* instruction level when it is told
    # nothing, and the C++ drivers take the floor - so a suite pinned to neither would assert
    # different things on different machines. See test/directives.h's note: a golden that changed
    # with the machine that ran it would assert nothing, and the same argument applies to a run.
    #
    # A row that names a higher level still gets it: `-enable-inst` takes the **highest** of
    # everything named, so `v2` here is the floor rather than a ceiling, and the rows below add to
    # it rather than fighting it.
    #
    # Not a formality, and what it cost is worth remembering: this pin is what hid the two v3
    # miscompiles of findings.md §70 - `Random.reals` drawing outside its own bounds was the symptom,
    # and the floor is why the rest of the suite said nothing. Both are fixed and the `v3` row below
    # is what stands over them now, so the floor is here for reproducibility alone: what a run
    # asserts must not depend on the machine that ran it, which is test/directives.h's argument for
    # the goldens applied to a run.
    if ! "$yana" -add base -main Base -package base -test -enable-inst v2 -to "$dir" "$@" > "$out/$name.build" 2>&1; then
        echo "== $name: the build failed"
        cat "$out/$name.build"
        return 1
    fi

    # A build that reported diagnostics and still produced something is a failure too - the corpus is
    # written to compile clean, so a new warning is a thing to look at rather than to scroll past.
    if [ -s "$out/$name.build" ] && grep -q "error:" "$out/$name.build"; then
        echo "== $name: the build reported errors"
        cat "$out/$name.build"
        return 1
    fi

    echo "== $name"

    if [ "$name" = "js" ] || [ "$name" = "js-unoptimized" ]; then
        if ! command -v node > /dev/null; then
            echo "   node is not on PATH, so the JavaScript rows are not being run."
            return 0
        fi
        node "$dir/Base.js" || return 1
    else
        "$dir/Base" || return 1
    fi
}

# The rows above v2 **execute** the instructions they compile - nothing here detects anything and
# nothing here dispatches - so a machine without them cannot run one. This is the same bargain
# `test/directives.h` describes for the fixtures these rows replaced, and the same shape as the
# `node` guard in run_row: a row that cannot run says so rather than being skipped in silence.
hostHas() {
    if [ ! -r /proc/cpuinfo ]; then return 1; fi
    grep -qw "$1" /proc/cpuinfo
}

status=0

run_row amd64          -mode exe -backend local                    || status=1
run_row generic        -mode exe -backend local -specialize never  || status=1
run_row unoptimized    -mode exe -backend local -no-ir-opt            || status=1
# **The rows above the floor.** Every boundary in `moveMemory`/`copyMemory` is stated in widths -
# the ladder's rungs are `n` transfers of `w` bytes covering `[nw, 2nw)`, the four-register group is
# `4w`, and the length above which the copy is handed to `blockCopy` is `48w`, so the sweeps cross a
# different one of them in each row. At v2 the vector rung starts at sixteen and meets the word rung
# below it exactly; at v3 it starts at thirty-two and leaves `[16, 32)` to a rung that exists only on
# a target whose register is wider than it is. See §"The rungs between sixteen bytes and the
# register" in Native.yana, which these rows are the only cover for.
#
# `sha` is beside the level rather than one of them, and it decides *file selection*: with it the
# selector reads `Digest/Hardware.sha.yana` instead of `Hardware.nosha.yana`, so every SHA-1 and
# SHA-256 answer comes from `sha1rnds4`/`sha256rnds2` rather than from the portable rounds. Nothing
# else in `Digest` changes, and all of it is asserted in both rows because the two files have to
# agree about all of it.
#
# v4 would want the same again for `[32, 64)`, and the rung is written; it needs a host with
# AVX-512 to run it.
if hostHas avx2; then
    run_row v3 -mode exe -backend local -enable-inst v3 || status=1

    if hostHas sha_ni; then
        run_row sha -mode exe -backend local -enable-inst v3 -enable-inst sha || status=1
    else
        echo "== sha: this machine has no SHA-NI, so that row is not being run."
    fi
else
    echo "== v3, sha: this machine has no AVX2, so neither row is being run."
fi

run_row js             -mode js                                    || status=1
run_row js-unoptimized -mode js -no-ir-opt                            || status=1

exit $status
