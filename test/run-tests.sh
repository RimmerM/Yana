#!/usr/bin/env bash
#
# Every test driver, at once.
#
# The drivers are independent processes over independent fixture directories, and the two large ones
# take a `shard:i/n` argument (see shard.h) that splits their corpus across as many processes as
# there are cores. Nothing here is a test of its own: it runs what the drivers run, and reports what
# they reported.
#
# The fixture paths in every driver are relative to this directory, so this cds here rather than
# trusting where it was called from.
#
#   ./run-tests.sh              # against ../build-assert, one job per core
#   JOBS=4 ./run-tests.sh       # four at a time
#   ./run-tests.sh ../build-release
#
# Passes any further arguments through to every driver, so `./run-tests.sh ../build-assert generate` is a
# whole-suite regenerate - read `git status test/` afterwards, as always.

set -u -o pipefail

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

build="${1:-../build-assert}"
shift 2>/dev/null || true

jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
logs="$(mktemp -d)"
trap 'rm -rf "$logs"' EXIT

if [ ! -x "$build/test/YanaResolveTest" ]; then
    echo "no drivers in $build/test - build them first, or name another build directory" >&2
    exit 1
fi

# One line per job: a name, then the command. The two sharded drivers get one job per core; the rest
# are a fraction of a second each and are simply thrown in alongside.
jobfile="$logs/jobs"
: > "$jobfile"

for i in $(seq 0 $((jobs - 1))); do
    echo "resolve.$i|$build/test/YanaResolveTest shard:$i/$jobs" >> "$jobfile"
done

# The parser corpus is 33 fixtures and the truncation test makes each of them cost far more than its
# size suggests, so it is worth sharding too - but never into more shards than there are fixtures.
parserJobs=$((jobs < 33 ? jobs : 33))
for i in $(seq 0 $((parserJobs - 1))); do
    echo "parser.$i|$build/test/YanaParseTest shard:$i/$parserJobs" >> "$jobfile"
done

# The library suite: every fixture compiled four or five times and run. Sharded on the same terms the
# two large drivers are, and never into more shards than there are fixtures - counted rather than
# written down, because the corpus grows and a stale number here silently caps the parallelism.
libFixtures=$(find lib -maxdepth 1 -name '*.yana' | wc -l)
libJobs=$((jobs < libFixtures ? jobs : libFixtures))
for i in $(seq 0 $((libJobs - 1))); do
    echo "lib.$i|$build/test/YanaLibTest shard:$i/$libJobs" >> "$jobfile"
done

# The standard library's own suite, which is not a driver at all - `lib/**/*.test.yana` are files of
# the modules they test, so what runs them is the compiler and the runner, over one program. Five
# builds of it, and each one draws its own report: see run-base.sh.
echo "base|./run-base.sh $build" >> "$jobfile"

echo "lower|$build/test/YanaLowerTest" >> "$jobfile"
echo "x64|$build/test/YanaX64Test" >> "$jobfile"
echo "llvm|$build/test/YanaLlvmTest" >> "$jobfile"

# Source to executable file to running process, over both fixture corpora - the resolve driver's and
# the library one. It compiles every runnable fixture, so it is sharded on the same terms they are.
for i in $(seq 0 $((jobs - 1))); do
    echo "elf.$i|$build/test/YanaElfTest shard:$i/$jobs" >> "$jobfile"
done

echo "edit|$build/test/YanaEditTest" >> "$jobfile"
echo "lsp|$build/test/YanaLspTest" >> "$jobfile"
echo "lsp-protocol|$build/test/YanaLspProtocolTest" >> "$jobfile"

extra=("$@")
export LOGS="$logs"
export EXTRA="${extra[*]:-}"

started=$(date +%s.%N)

# `xargs` exits 123 if any command failed, which is the whole result this needs. Each job's output
# goes to a file of its own so that concurrent drivers do not interleave into an unreadable log.
tr '\n' '\0' < "$jobfile" | xargs -0 -P "$jobs" -I{} bash -c '
    line="{}"
    name="${line%%|*}"
    command="${line#*|}"
    # shellcheck disable=SC2086
    $command $EXTRA > "$LOGS/$name.log" 2>&1
    status=$?
    echo "$status" > "$LOGS/$name.status"
    exit $status
'
result=$?

finished=$(date +%s.%N)

failed=()
for status in "$logs"/*.status; do
    [ -e "$status" ] || continue
    if [ "$(cat "$status")" != "0" ]; then
        name="$(basename "$status" .status)"
        failed+=("$name")
    fi
done

if [ ${#failed[@]} -ne 0 ]; then
    for name in "${failed[@]}"; do
        echo "=============== $name"
        cat "$logs/$name.log"
    done
fi

passed=$(cat "$logs"/*.log 2>/dev/null | grep -c "Pass\.$")
echo
printf 'ran %s jobs in %.1fs - %s passing\n' "$(wc -l < "$jobfile")" \
       "$(echo "$finished - $started" | bc)" "$passed"

if [ ${#failed[@]} -ne 0 ]; then
    echo "FAILED: ${failed[*]}"
    exit 1
fi

echo "all green"
exit $((result != 0))
