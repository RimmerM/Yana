#pragma once

#include <Core.h>
#include <cstdlib>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/*
 * `shard:i/n` - the argument that lets one fixture directory be run by several processes at once.
 *
 * The fixtures in a directory are independent: each is compiled from its own source, asserted
 * against its own expectation files, and the only thing any of them writes is its own output. What
 * kept the suite serial was therefore nothing about the tests - it was that a driver is one process
 * and a directory listing is one loop.
 *
 * Shard `i` of `n` runs the fixtures whose position in the listing is `i` modulo `n`. Round-robin
 * rather than contiguous blocks because a listing is alphabetical, and neither the size of a fixture
 * nor the number of modes it opts into has anything to do with its initial letter - blocks put the
 * whole `Array*` family in one process and left another with `X` and `W`.
 *
 * Returns false for an argument that is not a shard spec, so a caller can go on to try its other
 * arguments against it.
 */
inline bool parseShard(const Tritium::String& arg, U32& shard, U32& shards) {
    using namespace Tritium;

    auto prefix = "shard:"_v;
    if(arg.size() <= prefix.length) return false;
    if(compareMem(arg.text(), prefix.ptr, prefix.length) != 0) return false;

    // Copied out because `String::text()` is a counted pointer with no terminator, and `strtoul`
    // reads until one.
    char spec[64];
    Size length = arg.size() - prefix.length < sizeof(spec) - 1 ? arg.size() - prefix.length : sizeof(spec) - 1;
    for(Size i = 0; i < length; i++) spec[i] = arg.text()[prefix.length + i];
    spec[length] = 0;

    char* rest = nullptr;
    auto index = strtoul(spec, &rest, 10);
    if(rest == spec || *rest != '/') return false;

    auto count = strtoul(rest + 1, &rest, 10);
    if(!count || index >= count) return false;

    shard = U32(index);
    shards = U32(count);
    return true;
}

/*
 * `jobs:n` - run this driver's own shards, `n` of them, in forked children.
 *
 * The same split `shard:i/n` makes and the same reason it exists; what it adds is that one command
 * uses the whole machine. `run-tests.sh` already shards every driver across the cores, so the suite
 * was never the slow case - the slow case is the loop a change is actually made in, where the driver
 * whose corpus was touched is run on its own and takes as long serially as the whole suite does in
 * parallel.
 *
 * Returns false for an argument that is not a jobs spec, on the same terms `parseShard` does.
 */
inline bool parseJobs(const Tritium::String& arg, U32& jobs) {
    using namespace Tritium;

    auto prefix = "jobs:"_v;
    if(arg.size() <= prefix.length) return false;
    if(compareMem(arg.text(), prefix.ptr, prefix.length) != 0) return false;

    char spec[64];
    Size length = arg.size() - prefix.length < sizeof(spec) - 1 ? arg.size() - prefix.length
                                                                : sizeof(spec) - 1;
    for(Size i = 0; i < length; i++) spec[i] = arg.text()[prefix.length + i];
    spec[length] = 0;

    char* rest = nullptr;
    auto count = strtoul(spec, &rest, 10);
    if(rest == spec || *rest != 0 || !count) return false;

    jobs = U32(count);
    return true;
}

/*
 * How many shards a driver runs when nothing on the command line said - one per core.
 *
 * The default rather than `1`, because the serial run was never the useful one: `run-tests.sh`
 * already shards every large driver across the machine, so a driver invoked on its own was the *only*
 * way to run a corpus serially, and it is exactly the invocation an edit loop uses. One core out of
 * however many is not a conservative default, it is the slow one.
 *
 * A driver given an explicit `shard:i/n` keeps `1`: its corpus was already split by whoever forked
 * it, and forking again would run each fixture in a shard of a shard. `chooseJobs` is where the two
 * are told apart, so no driver decides it twice.
 */
inline U32 defaultJobs() {
#if defined(__unix__) || defined(__APPLE__)
    auto cores = sysconf(_SC_NPROCESSORS_ONLN);
    return cores > 1 ? U32(cores) : 1;
#else
    return 1;
#endif
}

/*
 * The job count a run actually uses, from what was written and whether a shard was named.
 *
 * `written` is zero when no `jobs:n` appeared - `parseJobs` refuses `jobs:0`, so zero cannot mean
 * anything else - and `jobs:1` is therefore how a caller asks for a single process, which is what a
 * debugger and a profiler want.
 */
inline U32 chooseJobs(U32 written, bool sharded) {
    if(written) return written;
    return sharded ? 1 : defaultJobs();
}

/*
 * The parent half of `jobs:n` - fork the shards, wait, and replay what they said.
 *
 * Returns a status for `main` to return in the **parent**, and `-1` in a **child**, having set that
 * child's `shard` and `shards`. So a driver's `main` reads
 *
 *     if(jobs > 1) { auto status = forkShards(jobs, shard, shards); if(status >= 0) return status; }
 *
 * placed before anything else it does, because everything a driver holds - a listing, a node
 * harness, an arena reservation - is cheaper to build once per child than to fork across.
 *
 * Each child's output goes to a file of its own and is replayed in shard order when it is waited
 * for. Not because interleaving is untidy: a failure prints the whole expected and actual dump, and
 * thirty-two of those interleaved line by line is a report nobody can read. The order is the shards'
 * and not completion order, so two runs of the same corpus print the same thing.
 */
inline int forkShards(U32 jobs, U32& shard, U32& shards) {
#if defined(__unix__) || defined(__APPLE__)
    using namespace Tritium;

    auto directory = getenv("TMPDIR");
    if(!directory || !*directory) directory = "/tmp";

    auto parent = getpid();

    struct Child {
        pid_t pid;
        char path[512];
    };

    Array<Child> children;

    for(U32 i = 0; i < jobs; i++) {
        Child child;
        snprintf(child.path, sizeof(child.path), "%s/yana-shard-%d-%u.log", directory, (int)parent,
                 i);

        auto log = open(child.path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if(log < 0) {
            println("cannot open %@ for a shard's output - running serially", String(child.path));
            return -1;
        }

        auto pid = fork();
        if(pid == 0) {
            // The child: its own stdout, its own slice, and back to `main` as an ordinary run.
            dup2(log, 1);
            dup2(log, 2);
            close(log);

            shard = i;
            shards = jobs;
            return -1;
        }

        close(log);

        if(pid < 0) {
            println("cannot fork a shard - running serially");

            // Whatever started is still ours to collect, so this falls through to the wait below
            // rather than leaving children behind and going on to run the corpus a second time.
            jobs = i;
            break;
        }

        child.pid = pid;
        children.push(child);
    }

    auto failed = false;

    for(auto& child: children) {
        int status = 0;
        waitpid(child.pid, &status, 0);
        if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) failed = true;

        auto log = fopen(child.path, "rb");
        if(log) {
            char buffer[4096];
            for(;;) {
                auto read = fread(buffer, 1, sizeof(buffer), log);
                if(!read) break;
                fwrite(buffer, 1, read, stdout);
            }

            fclose(log);
        }

        unlink(child.path);
    }

    fflush(stdout);
    return failed ? 1 : 0;
#else
    (void)jobs; (void)shard; (void)shards;
    return -1;
#endif
}

/*
 * An argument shaped like a flag - `-v`, `--shard=0/4`, `-generate`.
 *
 * These drivers take bare words: `generate`, `shard:i/n`, and a fixture-name prefix. Anything else
 * *became* the prefix, so a mistyped or borrowed flag selected no fixtures at all and the run
 * reported "no tests found" - which reads as a broken checkout rather than as a bad argument. And
 * `run-tests.sh` passes everything after the build directory to every driver, so one flag meant for
 * one of them was silently swallowed by the rest.
 *
 * A leading dash is never a fixture name, so it is the one shape that can be refused with certainty.
 * The message names what is accepted, since that is the thing a caller could not have guessed.
 */
inline bool rejectFlagArgument(const Tritium::String& arg, const char* extra = nullptr) {
    if(arg.size() == 0 || arg.text()[0] != '-') return false;

    if(extra) {
        Tritium::println("unknown argument \"%@\" - this driver takes `generate`, `shard:i/n`, `jobs:n`, %@, or a fixture name prefix",
                         arg, Tritium::String(extra));
    } else {
        Tritium::println("unknown argument \"%@\" - this driver takes `generate`, `shard:i/n`, `jobs:n`, or a fixture name prefix",
                         arg);
    }

    return true;
}
