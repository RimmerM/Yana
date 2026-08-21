#pragma once

#include <Core.h>
#include <File.h>

/*
 * `lib/` is two directories, and this is how the drivers tell them apart.
 *
 * The library fixture corpus is `test/lib`, and every driver reads its fixtures by paths relative to
 * the working directory - so `lib/` means the corpus when the driver was run from `test/` and the
 * *standard library* when it was run from the repository root, where `lib/Core.yana` is.
 *
 * The three drivers that read the corpus - `YanaLibTest`, `YanaElfTest` and `YanaBench` - would
 * otherwise compile `Core`, `Native` and `Host` as though they were fixtures, which is a hundred and
 * fifteen diagnostics and a red run that says nothing. The "a driver that verified nothing must not
 * be mistaken for one that verified everything" guard in each of them cannot catch it, because the
 * directory it found was not empty.
 *
 * `Core.yana` is the file to look for because it is the one every candidate library directory is
 * judged by elsewhere too - see `holdsLibrary` in compiler/compiler/library.cpp.
 */
inline bool libraryCorpusIsTheStandardLibrary() {
    auto info = Tritium::File::info(Tritium::String("lib/Core.yana"));
    return info && !info.unwrapOk().isDirectory;
}

inline bool reportWrongDirectory() {
    if(!libraryCorpusIsTheStandardLibrary()) return false;

    Tritium::println("`lib/` here is the standard library, not the fixture corpus - run this from test/");
    return true;
}
