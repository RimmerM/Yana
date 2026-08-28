#pragma once

#include <Core.h>
#include <cstdlib>

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
        Tritium::println("unknown argument \"%@\" - this driver takes `generate`, `shard:i/n`, %@, or a fixture name prefix",
                         arg, Tritium::String(extra));
    } else {
        Tritium::println("unknown argument \"%@\" - this driver takes `generate`, `shard:i/n`, or a fixture name prefix",
                         arg);
    }

    return true;
}
