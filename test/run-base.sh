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
    # most of the row's wall time and the run it produces is ~70ms, so a row that named itself only
    # once it had something to report would put its whole log out at the end with nothing before it.
    # It goes to the row's own log rather than to the terminal - see `start_row`.
    echo "== $name: building"

    # **sse4, explicitly.** The command line takes the *host's* instruction level when it is told
    # nothing, and the C++ drivers take the floor - so a suite pinned to neither would assert
    # different things on different machines. See test/directives.h's note: a golden that changed
    # with the machine that ran it would assert nothing, and the same argument applies to a run.
    #
    # A row that names a higher level still gets it: `-enable-inst` takes the **highest** of
    # everything named, so `sse4` here is the floor rather than a ceiling, and the rows below add to
    # it rather than fighting it.
    #
    # Not a formality, and what it cost is worth remembering: this pin is what hid the two v3
    # miscompiles of findings.md §70 - `Random.reals` drawing outside its own bounds was the symptom,
    # and the floor is why the rest of the suite said nothing. Both are fixed and the `avx2` row below
    # is what stands over them now, so the floor is here for reproducibility alone: what a run
    # asserts must not depend on the machine that ran it, which is test/directives.h's argument for
    # the goldens applied to a run.
    if ! "$yana" -add base -main Base -package base -test -enable-inst sse4 -to "$dir" "$@" > "$out/$name.build" 2>&1; then
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

    case "$name" in
        legacy-js|legacy-js-unoptimized|ownership-js|ownership-js-unoptimized|ownership-js-no-jsir-opt)
            isJs=1 ;;
        *) isJs=0 ;;
    esac

    if [ "$isJs" = "1" ]; then
        if ! command -v node > /dev/null; then
            echo "   node is not on PATH, so the JavaScript rows are not being run."
            return 0
        fi
        node "$dir/Base.js" || return 1
    else
        "$dir/Base" || return 1
    fi
}

# The rows above sse4 **execute** the instructions they compile - nothing here detects anything and
# nothing here dispatches - so a machine without them cannot run one. This is the same bargain
# `test/directives.h` describes for the fixtures these rows replaced, and the same shape as the
# `node` guard in run_row: a row that cannot run says so rather than being skipped in silence.
hostHas() {
    if [ ! -r /proc/cpuinfo ]; then return 1; fi
    grep -qw "$1" /proc/cpuinfo
}

# The rows, at once.
#
# Every row builds into a directory of its own from the same sources and runs what it built, so
# nothing any of them does is visible to another - which makes the sequence a sequence for no reason.
# It was the whole suite's critical path: `run-tests.sh` shards its two large drivers across the
# cores and finishes them in seconds, and then waited about two minutes for these seven rows one
# after another.
#
# Each row's output goes to a log of its own and is printed when that row is waited for, in the order
# the rows are declared rather than the order they finish. Two reasons, and the second is the one
# that matters: a report drawn by the runner is a block of lines meant to be read together, and seven
# of them interleaved line by line is not a report at all. The first is that a suite whose output
# order depends on which row won a race is one whose diff against a previous run says nothing.
names=()
pids=()

start_row() {
    local name="$1"; shift

    run_row "$name" "$@" > "$out/$name.log" 2>&1 &

    names+=("$name")
    pids+=("$!")
}

# A row that is not being run, in the same stream and the same order as the rows that are. A skip
# printed straight to the terminal would come out before every row's log rather than in its place.
note() {
    local name="$1"; shift

    printf '%s\n' "$*" > "$out/$name.log"
    names+=("$name")
    pids+=(0)
}

status=0

# **Every row names its stage.** `-resolver` defaults to the rewrite since 2026-09-22 (J81), and a
# matrix whose rows inherit that default is a matrix that changes which compiler it is about when the
# default moves - which is the one thing a comparison suite must not do. So the rows are prefixed
# with the stage they build, and each one writes the flag out even where it agrees with the default.
start_row legacy         -mode exe -backend local -resolver legacy
start_row legacy-generic -mode exe -backend local -specialize never -resolver legacy
start_row legacy-unoptimized -mode exe -backend local -no-ir-opt -resolver legacy

# **Every baseline and no replacement** - Design-Replacements.md §7. A replacement is held to answering
# exactly what its baseline answers, and nothing can prove that; what checks it is the same cases
# passing on both bodies. The rows above choose every replacement their target admits - the vector
# bodies of `Core/Reduce.yana` in every one of them, and the SHA instructions in `sha` - and this row
# chooses none, so a replacement that disagrees with its baseline fails one of the two. One row is
# enough whatever the host: a baseline does not depend on the level, and the replacements it is
# compared with are the other rows'.
start_row legacy-no-replacements -mode exe -backend local -no-replacements -resolver legacy

# **The rows above the floor.** Every boundary in `moveMemory`/`copyMemory` is stated in widths -
# the ladder's rungs are `n` transfers of `w` bytes covering `[nw, 2nw)`, the four-register group is
# `4w`, and the length above which the copy is handed to `blockCopy` is `48w`, so the sweeps cross a
# different one of them in each row. At sse4 the vector rung starts at sixteen and meets the word rung
# below it exactly; at avx2 it starts at thirty-two and leaves `[16, 32)` to a rung that exists only on
# a target whose register is wider than it is. See §"The rungs between sixteen bytes and the
# register" in Native.yana, which these rows are the only cover for.
#
# `sha` is beside the level rather than one of them, and it decides *file selection*: with it the
# selector reads `Digest/ShaBlock.sha.avx2.yana`, whose replacements are chosen over the portable
# loops, so every SHA-1 and SHA-256 answer comes from `sha1rnds4`/`sha256rnds2` rather than from the
# portable rounds. Nothing else in `Digest` changes, and all of it is asserted in both rows because
# the two bodies have to agree about all of it.
#
# `avx512` is the third level, and two rows. At its own natural width a vector is 64 bytes -
# Analysis-Avx512.md §1.2's stage 2 - which is the third set of widths for the ladder above and the
# first build in which `splitFullVectors` runs anything as two halves, and where the rungs in
# `[32, 64)` are reached. Held at 256 bits by `-vector-width` it is stage 1 again: a vector is what it
# is at `avx2`, but every instruction under it may be EVEX-encoded, name one of the upper sixteen
# vector registers, or be a mask register, so every case there asserts that the second set of
# expansions agrees with the first. The level is the Ice Lake and Zen 4 intersection, so the host has
# to have every extension of it and every flag it implies; `/proc/cpuinfo` spells two of them with
# underscores.
avx512Flags="avx512f avx512cd avx512bw avx512dq avx512vl avx512ifma avx512vbmi avx512_vbmi2 avx512_bitalg
             avx512_vpopcntdq avx512_vnni vaes vpclmulqdq gfni sha_ni"

hostHasAll() {
    for flag in "$@"; do hostHas "$flag" || return 1; done
}

# The ladder for one stage - the stage is the rows' prefix and the last flag of each, which is how
# every row here names its stage. A function since J133, when the rewrite got the same ladder: the
# host checks are the part worth writing once, because two copies of them that disagreed would run a
# row for one stage that the other skipped in silence.
start_levels() {
    local stage="$1"

    if hostHasAll avx2 aes pclmulqdq; then
        start_row "$stage-avx2" -mode exe -backend local -enable-inst avx2 -resolver "$stage"

        if hostHas sha_ni; then
            start_row "$stage-sha" -mode exe -backend local -enable-inst avx2 -enable-inst sha -resolver "$stage"
        else
            note "$stage-sha" "== $stage-sha: this machine has no SHA-NI, so that row is not being run."
        fi

        # shellcheck disable=SC2086 # the list is words on purpose
        if hostHasAll $avx512Flags; then
            start_row "$stage-avx512" -mode exe -backend local -enable-inst avx512 -resolver "$stage"
            start_row "$stage-avx512-256" -mode exe -backend local -enable-inst avx512 -vector-width 256 -resolver "$stage"
        else
            note "$stage-avx512" "== $stage-avx512, $stage-avx512-256: this machine is not an Ice Lake or Zen 4 or later, so neither row is being run."
        fi
    else
        note "$stage-avx2" "== $stage-avx2, $stage-sha, $stage-avx512: this machine has no AVX2 with AES-NI, so none of those rows is being run."
    fi
}

start_levels legacy

start_row legacy-js      -mode js -resolver legacy
start_row legacy-js-unoptimized -mode js -no-ir-opt -resolver legacy

# **The same suite through the rewritten resolver, natively.** It was deliberately kept out of this
# file while it was red - a row that always fails is a row everybody learns to ignore - and it has
# passed every case in both build trees since 2026-09-21 (J70), so since 2026-09-22 (J81, the day the
# rewrite became the default) it is a row. It was one row rather than the ladder above until J133 -
# see below for why that argument did not hold.
start_row ownership      -mode exe -backend local -enable-inst sse4 -resolver ownership

# **And the ladder**, since 2026-09-24 (J133) - the rows above the floor and the one without
# replacements, each the partner of the `legacy-*` row of its name. Until then the rewrite had one row
# here, on the argument that the ladder holds the library's *replacement* bodies to their baselines
# and those are selected the same way by either stage, so the `legacy-*` rows already asserted them.
# That was true of the selection and not of the code: the bodies a replacement selects are compiled by
# whichever stage is running, and every vector width, every EVEX encoding and every `sha` round the
# ladder reaches was reaching the x64 backend through the old stage's lowering alone. J94 §5 ran all
# five by hand and they passed; they are rows so that they keep passing. `-specialize never` has no
# partner, because the rewrite refuses it (J98): it has no erased mode to force.
start_row ownership-no-replacements -mode exe -backend local -no-replacements -resolver ownership
start_levels ownership

# **And unoptimized**, since 2026-09-23 (J95) - the native half of the differential the two JSIR rows
# below already ask. Without it a representation defect the optimizer folds away has nowhere to show,
# and one did for two days: J70 gave a continuation's captured window the binding's form, natively a
# window *by value*, and every row above scalar-replaced the environment before anything read it.
# `legacy-unoptimized` is the same row for the other stage. It was also the first input to stop in
# the x64 allocation verifier on a folded address whose index register an operand's window reused -
# which `build-release` compiled silently and answered right by luck - and that is
# `doc/progress/2026-09-23-J97-x64-folded-address.md`; it passes in both trees since.
start_row ownership-unoptimized -mode exe -backend local -no-ir-opt -resolver ownership

# **The same suite through the rewritten resolver and JSIR** - Analysis-IR-JS.md §9.7 item 8, and
# `doc/progress/2026-09-21-J69-jsir-erased-base-suite.md`.
#
# Two rows since J70, which is §10.2's differential asked of a whole program rather than of a
# fixture: the optimized row is what a build ships and the unoptimized one is the same program with
# `compiler/opt` switched off, and a representation defect that the optimizer happens to fold away
# shows in exactly one of them. The unoptimized row is where J67 §5's occurrence defect lived - a
# continuation's captured field built at the owner's representation rather than at the window's -
# and `doc/progress/2026-09-21-J70-jsir-correctness.md` is what closed it.
start_row ownership-js   -mode js -resolver ownership
start_row ownership-js-unoptimized -mode js -resolver ownership -no-ir-opt

# And the third configuration Analysis-IR-JS.md §10.2 asks for, added 2026-09-22 (J72): the shared
# optimizer on and the **JSIR** one off. The pair with the row above it is what says which tier a
# disagreement came from - a suite that differs with `-no-ir-opt` and agrees with this one has a
# defect above the fork, and the other way round has one below it, which is where
# `resolve_ssa/js/opt_*.cpp`'s rewrites are.
start_row ownership-js-no-jsir-opt -mode js -resolver ownership -no-jsir-opt

# Waited for and printed in declaration order, so the report comes out as the rows finish rather than
# all at the end, and comes out the same way twice.
for i in "${!names[@]}"; do
    pid="${pids[$i]}"
    if [ "$pid" != "0" ]; then
        wait "$pid" || status=1
    fi

    cat "$out/${names[$i]}.log"
done

exit $status
