#!/usr/bin/env python3
"""
The Unicode library's data - Implementation-String.md part 8.

Reads a pinned version of the Unicode Character Database, and CLDR's root collation, and writes the
tables `lib/Unicode` looks characters up in and the conformance data its tests walk. Every output is
checked in, so building the library never runs this; it is run when a pinned version changes, or
when a property is added:

    python3 tools/unicode/generate.py                 # fetch the pinned files into the cache, write
    python3 tools/unicode/generate.py --ucd DIR       # read an already-downloaded copy instead

It writes

    lib/Unicode/Tables.yana                   the property and cluster break tables and their lookups
    lib/Unicode/CaseTables.yana               full case mappings (part 8.3)
    lib/Unicode/NormalizationTables.yana      decompositions, compositions, quick checks (part 8.4)
    lib/Unicode/CollationTables.yana          CLDR's root collation elements (part 8.5)
    lib/Unicode/Tables.test.yana              every code point's expected property word and break
                                              class, as ranges, walked through the lookups
    lib/Unicode/Tries.test.yana               a checksum per 0x1000 code points of the other three tries
    lib/Unicode/GraphemeBreakTest.test.yana   GraphemeBreakTest.txt's cases, for `Clusters.test.yana`
    lib/Unicode/CaseTest.test.yana            every changed case mapping and NFKC_Casefold value
    lib/Unicode/NormalizationTest.test.yana   NormalizationTest.txt's cases
    lib/Unicode/CollationTest.test.yana       CLDR's CollationTest_CLDR_NON_IGNORABLE_SHORT.txt, in order

**The version is what current V8 ships**, so the host-backed collation of part 8.5 and this library
disagree as rarely as possible: Node 26 reports ICU 78, Unicode 17.0 and CLDR 48. Moving it is an
edit to `VERSION` and `CHECKSUMS`, and to `CLDR_VERSION` and `CLDR_CHECKSUMS`, below. The UCD's files
are fetched from unicode.org and CLDR's from its release tag, and each is refused unless its SHA-256
matches, so two runs at one version always read the same bytes.

**One table for every per-character property, not one per property.** A character's general
category and its binary properties are packed into one 16-bit *property word*, the distinct words
(about fifty) are stored once, and the trie maps a code point to an index into them. A caller asking
`isAlphabetic` pays for the general category too, which is 20 KB rather than the 4-8 KB one bitset
would be - but every query is the same three loads, and a program that asks two questions carries one
table rather than two. Properties with a different shape get tables of their own through `Trie`
below: the grapheme cluster break class, the case mappings, the normalization entries and the
collation elements.

**Three levels, with the split searched.** The shifts are chosen by minimizing the stored size; a
fourth level saves about a fifth of it (16.7 KB against 20.5 KB for the property words) and costs a
load on every lookup, which is not the trade a per-character predicate wants.
"""

import argparse
import hashlib
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
LIBRARY = os.path.join(ROOT, "lib", "Unicode")

VERSION = "17.0.0"

# The files read, where each lives under `ucd/`, and its SHA-256 at `VERSION`.
CHECKSUMS = {
    "extracted/DerivedGeneralCategory.txt": "d62e5bab70ca74f099343f71224fa051cb1fdd61a1ab45c0488c44cfc0b6102e",
    "DerivedCoreProperties.txt": "24c7fed1195c482faaefd5c1e7eb821c5ee1fb6de07ecdbaa64b56a99da22c08",
    "PropList.txt": "130dcddcaadaf071008bdfce1e7743e04fdfbc910886f017d9f9ac931d8c64dd",
    "auxiliary/GraphemeBreakProperty.txt": "d6b51d1d2ae5c33b451b7ed994b48f1f4dc62b2272a5831e7fd418514a6bae89",
    "auxiliary/GraphemeBreakTest.txt": "e2d134d2c52919bace503ebb6a551c1855fe1a1faec18478c78fff254a1793ec",
    "emoji/emoji-data.txt": "2cb2bb9455cda83e8481541ecf5b6dfda66a3bb89efa3fa7c5297eccf607b72b",
    "UnicodeData.txt": "2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c",
    "SpecialCasing.txt": "efc25faf19de21b92c1194c111c932e03d2a5eaf18194e33f1156e96de4c9588",
    "CaseFolding.txt": "ff8d8fefbf123574205085d6714c36149eb946d717a0c585c27f0f4ef58c4183",
    "DerivedNormalizationProps.txt": "71fd6a206a2c0cdd41feb6b7f656aa31091db45e9cedc926985d718397f9e488",
    "NormalizationTest.txt": "5019ffd530751a741900c849c0e010332f142a3612234639bd200b82138a87db",
}

# The root collation order is CLDR's rather than the UCD's DUCET, because CLDR's is what ICU - and so
# the host collator of part 8.5 - sorts by. CLDR 48 is the release at UCA 17.0.0, and is what ICU 78
# ships. Its files are fetched from the CLDR repository's release tag rather than from unicode.org,
# which publishes only the DUCET.
CLDR_VERSION = "48"
CLDR_CHECKSUMS = {
    "allkeys_CLDR.txt": "f6f8961f6c51373bdc00b09fb61cc8c07bec4122bc4b8bcb50ff71c4b0901ab5",
    "CollationTest_CLDR_NON_IGNORABLE_SHORT.txt": "7063c05ad292803f8d4269abc64e2ce50ab96d0038f799670ed325f9a5e20bb8",
}

CODE_POINTS = 0x110000

# `GeneralCategory`'s constructors in declaration order, which is the number the property word holds.
# The order is UnicodeData.txt's own grouping (letters, marks, numbers, punctuation, symbols,
# separators, others), so each group is a contiguous run and `Unicode.yana`'s group tests are ranges.
CATEGORIES = [
    ("Lu", "UppercaseLetter"), ("Ll", "LowercaseLetter"), ("Lt", "TitlecaseLetter"),
    ("Lm", "ModifierLetter"), ("Lo", "OtherLetter"),
    ("Mn", "NonspacingMark"), ("Mc", "SpacingMark"), ("Me", "EnclosingMark"),
    ("Nd", "DecimalNumber"), ("Nl", "LetterNumber"), ("No", "OtherNumber"),
    ("Pc", "ConnectorPunctuation"), ("Pd", "DashPunctuation"), ("Ps", "OpenPunctuation"),
    ("Pe", "ClosePunctuation"), ("Pi", "InitialPunctuation"), ("Pf", "FinalPunctuation"),
    ("Po", "OtherPunctuation"),
    ("Sm", "MathSymbol"), ("Sc", "CurrencySymbol"), ("Sk", "ModifierSymbol"), ("So", "OtherSymbol"),
    ("Zs", "SpaceSeparator"), ("Zl", "LineSeparator"), ("Zp", "ParagraphSeparator"),
    ("Cc", "Control"), ("Cf", "Format"), ("Cs", "Surrogate"), ("Co", "PrivateUse"),
    ("Cn", "Unassigned"),
]

CATEGORY_BITS = 5
UNASSIGNED = len(CATEGORIES) - 1

# The binary properties in the property word, above the category: (UCD name, bit, Yana constant).
FLAGS = [
    ("Alphabetic", 5, "alphabeticBit"),
    ("Lowercase", 6, "lowercaseBit"),
    ("Uppercase", 7, "uppercaseBit"),
    ("XID_Start", 8, "xidStartBit"),
    ("XID_Continue", 9, "xidContinueBit"),
    ("Cased", 10, "casedBit"),
    ("Case_Ignorable", 11, "caseIgnorableBit"),
    ("Default_Ignorable_Code_Point", 12, "defaultIgnorableBit"),
]

# The grapheme cluster break classes, in the order `Clusters.yana` numbers them - its rules are
# written over these numbers, so the two lists move together. Each is a `Grapheme_Cluster_Break`
# value, split where UAX #29's GB9c and GB11 need more than it says: `Extend` by
# `Indic_Conjunct_Break`, and `Other` into its `Extended_Pictographic` and `InCB=Consonant` parts.
# `generate` checks that the splits are partitions, which is also what lets `Clusters.yana` restart a
# scan at a boundary no context rule can reach across.
CLUSTER_CLASSES = [
    "Other", "CR", "LF", "Control",
    "Extend", "ConjunctExtend", "ConjunctLinker", "ZWJ", "SpacingMark",
    "Prepend", "Regional_Indicator", "L", "V", "T", "LV", "LVT",
    "Pictographic", "Consonant",
]

# Core's `isSpaceUnicode` writes this set out by hand (lib/Core/Text.yana); the generator is what
# notices if a Unicode version ever changes it.
WHITE_SPACE = [0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0x85, 0xA0, 0x1680] + list(range(0x2000, 0x200B)) + \
              [0x2028, 0x2029, 0x202F, 0x205F, 0x3000]


def sources():
    """Every file read, as (name, SHA-256, URL)."""
    for name, digest in CHECKSUMS.items():
        yield name, digest, f"https://www.unicode.org/Public/{VERSION}/ucd/{name}"
    for name, digest in CLDR_CHECKSUMS.items():
        yield name, digest, f"https://raw.githubusercontent.com/unicode-org/cldr/release-{CLDR_VERSION}/common/uca/{name}"


def fetch(cache):
    os.makedirs(cache, exist_ok=True)
    for name, digest, url in sources():
        path = os.path.join(cache, os.path.basename(name))
        if not os.path.exists(path):
            print(f"fetching {url}")
            with urllib.request.urlopen(url) as response:
                data = response.read()
            with open(path, "wb") as out:
                out.write(data)
        check(path, digest)


def open_source(ucd, name):
    """A pinned file, refused unless its SHA-256 is the one pinned."""
    digests = {**CHECKSUMS, **CLDR_CHECKSUMS}
    path = os.path.join(ucd, os.path.basename(name))
    check(path, digests[name])
    return open(path, encoding="utf-8")


def check(path, digest):
    with open(path, "rb") as f:
        actual = hashlib.sha256(f.read()).hexdigest()
    if actual != digest:
        sys.exit(f"{path}: SHA-256 is {actual}, expected {digest} for Unicode {VERSION}")


def ranges(ucd, name):
    """Every `start..end ; value ; ...` line of a UCD file, as (start, end, value, [the fields after])."""
    with open_source(ucd, name) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = [field.strip() for field in line.split(";")]
            start, _, end = fields[0].partition("..")
            yield int(start, 16), int(end or start, 16), fields[1], fields[2:]


def property_words(ucd):
    words = [UNASSIGNED] * CODE_POINTS
    names = [short for short, _ in CATEGORIES]
    for start, end, value, _ in ranges(ucd, "extracted/DerivedGeneralCategory.txt"):
        category = names.index(value)
        for c in range(start, end + 1):
            words[c] = category

    bits = {name: bit for name, bit, _ in FLAGS}
    for start, end, value, _ in ranges(ucd, "DerivedCoreProperties.txt"):
        if value in bits:
            for c in range(start, end + 1):
                words[c] |= 1 << bits[value]

    return words


def check_white_space(ucd):
    found = []
    for start, end, value, _ in ranges(ucd, "PropList.txt"):
        if value == "White_Space":
            found.extend(range(start, end + 1))
    if sorted(found) != WHITE_SPACE:
        sys.exit("White_Space changed in this version: update `isSpaceUnicode` in lib/Core/Text.yana "
                 "and WHITE_SPACE here")


def cluster_classes(ucd):
    names = {name: i for i, name in enumerate(CLUSTER_CLASSES)}
    classes = [names["Other"]] * CODE_POINTS
    for start, end, value, _ in ranges(ucd, "auxiliary/GraphemeBreakProperty.txt"):
        for c in range(start, end + 1):
            classes[c] = names[value]

    def split(c, within, into):
        if classes[c] not in [names[name] for name in within]:
            sys.exit(f"U+{c:04X} is {CLUSTER_CLASSES[classes[c]]}, which `{into}` does not split")
        classes[c] = names[into]

    for start, end, value, _ in ranges(ucd, "emoji/emoji-data.txt"):
        if value == "Extended_Pictographic":
            for c in range(start, end + 1):
                split(c, ["Other"], "Pictographic")

    conjunct = {"Consonant": (["Other"], "Consonant"), "Linker": (["Extend"], "ConjunctLinker"),
                "Extend": (["Extend"], "ConjunctExtend")}
    for start, end, value, rest in ranges(ucd, "DerivedCoreProperties.txt"):
        if value == "InCB":
            within, into = conjunct[rest[0]]
            for c in range(start, end + 1):
                # ZWJ is `InCB=Extend` and keeps its own class, since GB11 needs it by name.
                if classes[c] != names["ZWJ"] or rest[0] != "Extend":
                    split(c, within, into)

    return classes


def grapheme_cases(ucd):
    """Each case of GraphemeBreakTest.txt, as (code points, the number of code points in each cluster)."""
    cases = []
    with open_source(ucd, "auxiliary/GraphemeBreakTest.txt") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            points = []
            sizes = []
            for token in line.split()[1:]:
                if token == "\u00f7":
                    sizes.append(len(points) - sum(sizes))
                elif token != "\u00d7":
                    points.append(int(token, 16))
            cases.append((points, sizes))
    return cases


def unicode_data(ucd):
    """UnicodeData.txt's fields that later steps read: combining classes, decompositions, simple cases."""
    ccc = {}
    canonical = {}
    compatibility = {}
    upper = {}
    lower = {}
    with open_source(ucd, "UnicodeData.txt") as f:
        for line in f:
            fields = line.rstrip("\n").split(";")
            c = int(fields[0], 16)
            if int(fields[3]):
                ccc[c] = int(fields[3])
            if fields[5]:
                parts = fields[5].split()
                if parts[0].startswith("<"):
                    compatibility[c] = [int(part, 16) for part in parts[1:]]
                else:
                    canonical[c] = [int(part, 16) for part in parts]
            if fields[12]:
                upper[c] = [int(fields[12], 16)]
            if fields[13]:
                lower[c] = [int(fields[13], 16)]
    return {"ccc": ccc, "canonical": canonical, "compatibility": compatibility, "upper": upper, "lower": lower}


# --------------------------------------------------------------------------------------------------
# Case - part 8.3.

# A mapping value at or above this is a run of `caseStrings` rather than a difference: the largest
# difference between a character and its mapping is well under 0x110000 either way.
CASE_STRING_TAG = 0x200000


def case_mappings(ucd, data):
    """
    Each code point's full lowercase, uppercase and case folding, as {code point: [code points]} for
    the ones that are not the character itself.

    Full mappings, locale-independent: SpecialCasing.txt's unconditional lines over UnicodeData.txt's
    simple ones, and CaseFolding.txt's `C` and `F` lines. A conditional line is dropped - the language
    ones are out of scope (part 8.3), and `Final_Sigma` is the one condition `Case.yana` implements,
    over the simple mapping this leaves in place.
    """
    lower = dict(data["lower"])
    upper = dict(data["upper"])
    final_sigma = False
    with open_source(ucd, "SpecialCasing.txt") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            fields = [field.strip() for field in line.split(";")]
            c = int(fields[0], 16)
            if fields[4]:
                final_sigma = final_sigma or (c == 0x03A3 and fields[4] == "Final_Sigma")
                continue
            lower[c] = [int(part, 16) for part in fields[1].split()]
            upper[c] = [int(part, 16) for part in fields[3].split()]
    if not final_sigma:
        sys.exit("SpecialCasing.txt no longer has U+03A3's Final_Sigma line, which Case.yana assumes")

    fold = {}
    for start, _, status, rest in ranges(ucd, "CaseFolding.txt"):
        if status in ("C", "F"):
            fold[start] = [int(part, 16) for part in rest[0].split()]

    def changed(mapping):
        return {c: value for c, value in mapping.items() if value != [c]}

    return changed(lower), changed(upper), changed(fold)


def case_tables(lower, upper, fold):
    """The case trie's values, the distinct entries it indexes, and the strings the entries point into."""
    strings = []
    offsets = {}

    def encode(c, mapping):
        value = mapping.get(c, [c])
        if len(value) == 1:
            return value[0] - c
        assert len(value) <= 3
        key = tuple(value)
        if key not in offsets:
            offsets[key] = len(strings)
            strings.extend(value)
        return CASE_STRING_TAG + (offsets[key] << 2) + len(value)

    entries = {(0, 0, 0): 0}
    index = [0] * CODE_POINTS
    for c in sorted(set(lower) | set(upper) | set(fold)):
        entry = (encode(c, lower), encode(c, upper), encode(c, fold))
        index[c] = entries.setdefault(entry, len(entries))
    return index, list(entries), strings


# --------------------------------------------------------------------------------------------------
# Normalization - part 8.4.

HANGUL_FIRST = 0xAC00
HANGUL_LAST = 0xD7A3

# The quick-check bits of a normalization entry, above its 8-bit combining class.
NORMALIZATION_FLAGS = [
    ("NFD_QC", "N", 8, "nfdNoBit"),
    ("NFKD_QC", "N", 9, "nfkdNoBit"),
    ("NFC_QC", "N", 10, "nfcNoBit"),
    ("NFC_QC", "M", 11, "nfcMaybeBit"),
    ("NFKC_QC", "N", 12, "nfkcNoBit"),
    ("NFKC_QC", "M", 13, "nfkcMaybeBit"),
]
NORMALIZATION_DATA_SHIFT = 14


def full_decomposition(c, data, compatible):
    mapping = data["canonical"].get(c)
    if mapping is None and compatible:
        mapping = data["compatibility"].get(c)
    if mapping is None:
        return [c]
    return [d for part in mapping for d in full_decomposition(part, data, compatible)]


def normalization_tables(ucd, data):
    """
    The normalization trie's values, the distinct entries, and the data the entries point into.

    An entry is a combining class, the quick-check bits, and an offset into `normalizationData`, where
    a header word says how many code points of full canonical decomposition, of full compatibility
    decomposition and of composition pairs follow it: `canonical | compatible << 5 | pairs << 10`. A
    compatibility decomposition is stored only where it differs from the canonical one. Each pair is
    the second character of a primary composite starting with this one, and the composite. Hangul
    syllables have no data: `Normalization.yana` computes them.
    """
    flags = [0] * CODE_POINTS
    exclusions = set()
    by_name = {}
    for name, value, bit, _ in NORMALIZATION_FLAGS:
        by_name[(name, value)] = bit
    for start, end, name, rest in ranges(ucd, "DerivedNormalizationProps.txt"):
        if name == "Full_Composition_Exclusion":
            exclusions.update(range(start, end + 1))
        elif (name, rest[0] if rest else None) in by_name:
            bit = by_name[(name, rest[0])]
            for c in range(start, end + 1):
                flags[c] |= 1 << bit

    pairs = {}
    for c, mapping in data["canonical"].items():
        if len(mapping) == 2 and c not in exclusions:
            pairs.setdefault(mapping[0], []).append((mapping[1], c))

    pool = [0]
    entries = {0: 0}
    index = [0] * CODE_POINTS
    for c in range(CODE_POINTS):
        ccc = data["ccc"].get(c, 0)
        canonical = []
        compatible = []
        if c in data["canonical"] or c in data["compatibility"]:
            canonical = full_decomposition(c, data, False) if c in data["canonical"] else []
            compatible = full_decomposition(c, data, True)
            if compatible == canonical:
                compatible = []
        composes = sorted(pairs.get(c, []))
        entry = ccc | flags[c]
        if canonical or compatible or composes:
            assert len(canonical) < 32 and len(compatible) < 32 and len(composes) < 32
            entry |= len(pool) << NORMALIZATION_DATA_SHIFT
            pool.append(len(canonical) | len(compatible) << 5 | len(composes) << 10)
            pool.extend(canonical)
            pool.extend(compatible)
            for second, composite in composes:
                pool.extend([second, composite])
        if c in (HANGUL_FIRST, HANGUL_LAST):
            assert entry == flags[c]
        index[c] = entries.setdefault(entry, len(entries))

    assert len(pool) < 1 << (32 - NORMALIZATION_DATA_SHIFT)
    return index, list(entries), pool


def normalization_cases(ucd):
    """NormalizationTest.txt: each case's five columns, and the code points part 1 lists."""
    cases = []
    listed = set()
    part = None
    with open_source(ucd, "NormalizationTest.txt") as f:
        for line in f:
            if line.startswith("@Part"):
                part = line.split()[0]
                continue
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            columns = [[int(point, 16) for point in column.split()] for column in line.split(";")[:5]]
            cases.append(columns)
            if part == "@Part1":
                listed.add(columns[0][0])
    return cases, listed


def nfkc_casefold(ucd):
    """DerivedNormalizationProps.txt's NFKC_Casefold, for the code points it changes."""
    mapping = {}
    for start, end, name, rest in ranges(ucd, "DerivedNormalizationProps.txt"):
        if name == "NFKC_CF":
            value = [int(point, 16) for point in rest[0].split()]
            for c in range(start, end + 1):
                mapping[c] = value
    return mapping


# --------------------------------------------------------------------------------------------------
# Collation - part 8.5.

# A collation element packed into 32 bits: the primary weight in the high 16, the secondary in the
# next 9, the tertiary in the next 5, whether it is variable, and a zero low bit - which is what tells
# an element in the trie from a tagged value. The generator refuses a table whose weights do not fit.
COLLATION_TAG_BITS = 4
COLLATION_IMPLICIT = 0
COLLATION_EXPANSION = 1
COLLATION_CONTRACTION = 2

# UTS #10 section 10.1's implicit weights for the siniform scripts: the first code point the second
# weight counts from. `@implicitweights` in the DUCET names the base weight of each range, and CLDR's
# table leaves the same ranges out.
SINIFORM_STARTS = [0x17000, 0x18800, 0x1B170, 0x18B00]

# The trie value of a character past the last code point: an unassigned one's implicit weights.
COLLATION_OUTSIDE = (0xFBC0 << 3) << COLLATION_TAG_BITS | COLLATION_IMPLICIT << 1 | 1
SINIFORM_RANGES = [
    (0x17000, 0x187FF, 0xFB00, 0x17000), (0x18800, 0x18AFF, 0xFB01, 0x18800),
    (0x18D00, 0x18D7F, 0xFB00, 0x17000), (0x18D80, 0x18DFF, 0xFB01, 0x18800),
    (0x1B170, 0x1B2FF, 0xFB02, 0x1B170), (0x18B00, 0x18CFF, 0xFB03, 0x18B00),
]


def collation_element(variable, primary, secondary, tertiary):
    if primary >= 1 << 16 or secondary >= 1 << 9 or tertiary >= 1 << 5:
        sys.exit(f"a collation element [{primary:04X}.{secondary:04X}.{tertiary:04X}] does not fit 32 bits")
    return primary << 16 | secondary << 7 | tertiary << 2 | variable << 1


def collation_table(ucd):
    """allkeys_CLDR.txt, as {code points: [packed elements]}."""
    table = {}
    with open_source(ucd, "allkeys_CLDR.txt") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line or line.startswith("@"):
                continue
            key, _, value = line.partition(";")
            elements = []
            for chunk in value.strip()[1:-1].split("]["):
                weights = chunk[1:].split(".")
                elements.append(collation_element(chunk[0] == "*", *(int(w, 16) for w in weights)))
            table[tuple(int(point, 16) for point in key.split())] = elements
    return table


def unified_ideographs(ucd):
    found = set()
    for start, end, value, _ in ranges(ucd, "PropList.txt"):
        if value == "Unified_Ideograph":
            found.update(range(start, end + 1))
    return found


def collation_tables(ucd):
    """
    The collation trie's values, the element pool and the contraction records.

    A trie value with a zero low bit is the character's one element. Otherwise it is
    `payload << 4 | kind << 1 | 1`:

    - **implicit** (`kind` 0): the character is not in the table and its two elements are computed
      (UTS #10 section 10.1). The payload is the base weight `<< 3` with, for a siniform script, the
      index plus one of the range start in `collationSiniformStarts`.
    - **expansion** (`kind` 1): `offset << 5 | count` elements of `collationElements`.
    - **contraction** (`kind` 2): the index of the character's head record in
      `collationContractions`: its own elements as `offset << 5 | count`, the number of longer
      sequences starting with it, and for each one `extra | count << 2 | offset << 7` - `extra` is how
      many code points follow the first - and those one or two code points (zero-filled), longest
      first.
    """
    table = collation_table(ucd)
    ideographs = unified_ideographs(ucd)
    elements = []
    contractions = []

    def pooled(values):
        offset = len(elements)
        elements.extend(values)
        assert len(values) < 32
        return offset << 5 | len(values)

    def tagged(kind, payload):
        return payload << COLLATION_TAG_BITS | kind << 1 | 1

    heads = {}
    for key in table:
        if len(key) > 1:
            heads.setdefault(key[0], []).append(key)

    values = [0] * CODE_POINTS
    for c in range(CODE_POINTS):
        if c in heads:
            if (c,) not in table:
                sys.exit(f"U+{c:04X} starts a contraction and has no elements of its own")
            record = len(contractions)
            longer = sorted(heads[c], key=lambda key: (-len(key), key))
            contractions.extend([pooled(table[(c,)]), len(longer)])
            for key in longer:
                assert len(key) <= 3
                reference = pooled(table[key])
                contractions.extend([len(key) - 1 | (reference & 31) << 2 | (reference >> 5) << 7,
                                     key[1], key[2] if len(key) > 2 else 0])
            values[c] = tagged(COLLATION_CONTRACTION, record)
        elif (c,) in table:
            ces = table[(c,)]
            values[c] = ces[0] if len(ces) == 1 else tagged(COLLATION_EXPANSION, pooled(ces))
        else:
            base = 0xFBC0
            start = 0
            if c in ideographs:
                core = 0x4E00 <= c <= 0x9FFF or 0xF900 <= c <= 0xFAFF
                base = 0xFB40 if core else 0xFB80
            for first, last, siniform, counted in SINIFORM_RANGES:
                if first <= c <= last:
                    base = siniform
                    start = SINIFORM_STARTS.index(counted) + 1
            values[c] = tagged(COLLATION_IMPLICIT, base << 3 | start)

    assert max(values) < 1 << 32
    ascii = []
    for c in range(0x80):
        if len(table[(c,)]) != 1 or any(key[0] == c and any(point < 0x80 for point in key[1:]) for key in table if len(key) > 1):
            sys.exit(f"U+{c:04X} is not one element in every all-ASCII text, which Collation.yana's ASCII path assumes")
        ascii.append(table[(c,)][0])
    return values, elements, contractions, ascii


def collation_cases(ucd):
    """CollationTest_CLDR_NON_IGNORABLE's lines in order, without the ones holding a surrogate."""
    cases = []
    skipped = 0
    with open_source(ucd, "CollationTest_CLDR_NON_IGNORABLE_SHORT.txt") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            points = [int(point, 16) for point in line.split(";")[0].split()]
            if any(0xD800 <= point <= 0xDFFF for point in points):
                skipped += 1
                continue
            cases.append(points)
    return cases, skipped


class Trie:
    """
    A three-level trie over one value per code point.

    `top[c >> (mid + leaf)]` names a middle block, the middle block's entry at the next `mid` bits
    names a leaf block, and the leaf block's entry at the low `leaf` bits is the value. Identical
    blocks at each level are stored once, which is where all of the compression comes from: every
    unassigned plane is one middle block, and every run of one category is one leaf block.
    """

    def __init__(self, values):
        best = None
        for leaf in range(2, 9):
            leaves, leaf_ids = dedupe(values, 1 << leaf)
            for mid in range(2, 9):
                if mid + leaf > 21:
                    continue
                mids, mid_ids = dedupe(leaf_ids, 1 << mid)
                size = len(mid_ids) * width(len(mids)) + \
                    len(mids) * (1 << mid) * width(len(leaves)) + \
                    len(leaves) * (1 << leaf) * width(max(values) + 1)
                if best is None or size < best[0]:
                    best = (size, mid, leaf, mid_ids, flatten(mids), flatten(leaves))

        self.size, self.mid_bits, self.leaf_bits, self.top, self.mids, self.leaves = best

    def lookup(self, c):
        mid = self.top[c >> (self.mid_bits + self.leaf_bits)]
        leaf = self.mids[(mid << self.mid_bits) + ((c >> self.leaf_bits) & ((1 << self.mid_bits) - 1))]
        return self.leaves[(leaf << self.leaf_bits) + (c & ((1 << self.leaf_bits) - 1))]


def dedupe(values, size):
    """Splits `values` into blocks of `size` and answers the distinct blocks and each block's index."""
    blocks = {}
    ids = []
    for start in range(0, len(values), size):
        block = values[start:start + size]
        block = tuple(block + [block[-1]] * (size - len(block)))
        ids.append(blocks.setdefault(block, len(blocks)))
    return list(blocks), ids


def flatten(blocks):
    return [value for block in blocks for value in block]


def width(count):
    return 1 if count <= 0x100 else 2 if count <= 0x10000 else 4


def element_type(count):
    return {1: "U8", 2: "U16", 4: "U32"}[width(count)]


def array(name, values, type_name):
    lines = ["   "]
    for value in values:
        item = f" {value},"
        if len(lines[-1]) + len(item) > 100:
            lines.append("   ")
        lines[-1] += item
    lines[-1] = lines[-1][:-1]
    return f"let {name} = [\n" + "\n".join(lines) + f"\n] :: [{type_name} *{len(values)}]\n"


def trie_source(name, trie, value_type, outside, meaning):
    """The three arrays of `trie` and `{name}At`, which looks a `Char` up in them."""
    mask_mid = (1 << trie.mid_bits) - 1
    mask_leaf = (1 << trie.leaf_bits) - 1
    return f"""{array(name + "Top", trie.top, element_type(len(trie.mids) >> trie.mid_bits))}
{array(name + "Middle", trie.mids, element_type(len(trie.leaves) >> trie.leaf_bits))}
{array(name + "Leaves", trie.leaves, value_type)}
-- A `Char` above U+10FFFF is not a code point (the constructor does not check), and answers
-- {meaning} rather than reading past the top level.
@inline(always)
fn {name}At(c: Char) -> {value_type}:
    if c.0 > 0x10FFFF then return {outside}

    let at = c :: Size
    let top = {name}Top[at `shr` {trie.mid_bits + trie.leaf_bits}] :: Size
    let middle = (top `shl` {trie.mid_bits}) + ((at `shr` {trie.leaf_bits}) `and` {mask_mid})
    let leaf = (({name}Middle[middle] :: Size) `shl` {trie.leaf_bits}) + (at `and` {mask_leaf})
    return {name}Leaves[leaf]
"""


def checked_trie(values):
    trie = Trie(values)
    for c in range(CODE_POINTS):
        assert trie.lookup(c) == values[c]
    return trie


def categories(words):
    lines = ["let propertyCategories = ["]
    for word in words:
        lines.append(f"    GeneralCategory.{CATEGORIES[word & ((1 << CATEGORY_BITS) - 1)][1]},")
    lines[-1] = lines[-1][:-1]
    lines.append(f"] :: [GeneralCategory *{len(words)}]")
    return "\n".join(lines) + "\n"


HEADER = f"""-- GENERATED by tools/unicode/generate.py from the Unicode Character Database {VERSION}.
-- Do not edit: change the generator and run it again.
"""


def write_tables(words, classes):
    sets = sorted(set(words))
    set_index = {word: i for i, word in enumerate(sets)}
    properties = checked_trie([set_index[word] for word in words])
    breaks = checked_trie(classes)

    property_size = properties.size + len(sets) * 2
    flags = "\n".join(f"let {constant} = {1 << bit} :: U16" for _, bit, constant in FLAGS)
    out = f"""{HEADER}
{{-
   The per-character tables - Implementation-String.md parts 8.1 and 8.2.

   Two three-level tries, each with the split below its top level that makes it smallest.

   - **A property word** is one character's general category in the low {CATEGORY_BITS} bits (the constructor
     number of `GeneralCategory`) and one bit per binary property above it. The {len(sets)} distinct
     words are `propertyWords`, and `propertyIndexAt` finds a code point's: {property_size} bytes in all.
   - **A cluster break class** is `Clusters.yana`'s number for the character, one of
     {len(CLUSTER_CLASSES)}, looked up by `clusterClassAt`: {breaks.size} bytes.
-}}

-- The version of the Unicode Standard every answer in this module is from.
pub let unicodeVersion = "{VERSION}"

let categoryMask = {(1 << CATEGORY_BITS) - 1} :: U16
{flags}

{array("propertyWords", sets, "U16")}
-- Each word's category as the constructor, so that `generalCategory` is a load rather than a
-- conversion - `fromValue` takes an `I64`, which is a `BigInt` on JS.
{categories(sets)}
{trie_source("propertyIndex", properties, element_type(len(sets)), set_index[UNASSIGNED], "the index of an unassigned word")}
@inline(always)
fn propertyWord(c: Char) -> U16 = propertyWords[propertyIndexAt(c) :: Size]

{trie_source("clusterClass", breaks, "U8", 0, "`Other`")}"""
    with open(os.path.join(LIBRARY, "Tables.yana"), "w") as f:
        f.write(out)

    return property_size, breaks.size


def runs(values):
    starts = []
    kept = []
    for c, value in enumerate(values):
        if not kept or kept[-1] != value:
            starts.append(c)
            kept.append(value)
    return starts, kept


def write_test(words, classes):
    word_starts, word_values = runs(words)
    class_starts, class_values = runs(classes)

    out = f"""{HEADER}
{{-
   Every code point's property word and cluster break class, checked through the tries.

   The expectation is a second encoding of the same data - the start of each run of one value, and
   the value - so what this checks is each trie's construction, the lookup arithmetic on each target
   and `propertyCategories` against the words, for all 0x110000 code points and not a sample. Whether
   the values say what the UCD says is `Unicode.test.yana`'s and `Clusters.test.yana`'s business.
-}}

import Test

{array("expectedWordStarts", word_starts, "U32")}
{array("expectedWords", word_values, "U16")}
{array("expectedClassStarts", class_starts, "U32")}
{array("expectedClasses", class_values, "U8")}
@test fn everyCodePoint():
    let &wordRun = 0 :: Size
    let &classRun = 0 :: Size
    let &failures = 0 :: Size

    for at in 0 .. (0x110000 :: U32):
        if wordRun + 1 < {len(word_starts)} && at == expectedWordStarts[wordRun + 1] then wordRun += 1
        if classRun + 1 < {len(class_starts)} && at == expectedClassStarts[classRun + 1] then classRun += 1

        let word = propertyWord(Char(at))
        if word != expectedWords[wordRun]:
            if failures < 8 then checkEqual(word, expectedWords[wordRun])
            failures += 1

        let category = valueOf(generalCategory(Char(at)))
        if category != ((word `and` categoryMask) :: Int):
            if failures < 8 then checkEqual(category, (word `and` categoryMask) :: Int)
            failures += 1

        let breakClass = clusterClassAt(Char(at))
        if breakClass != expectedClasses[classRun]:
            if failures < 8 then checkEqual(breakClass, expectedClasses[classRun])
            failures += 1

    checkEqual(failures, 0)

@test fn pastTheLastCodePoint():
    checkEqual(propertyIndexAt(Char(0x110000)), {sorted(set(words)).index(UNASSIGNED)})
    checkEqual(propertyIndexAt(Char(0xFFFFFFFF)), {sorted(set(words)).index(UNASSIGNED)})
    checkEqual(clusterClassAt(Char(0x110000)), 0)
    checkEqual(clusterClassAt(Char(0xFFFFFFFF)), 0)
"""
    with open(os.path.join(LIBRARY, "Tables.test.yana"), "w") as f:
        f.write(out)

    return len(word_starts), len(class_starts)


def write_grapheme_cases(cases):
    def literal(points):
        return '"' + "".join(f"\\x{c:X}" for c in points) + '"'

    texts = ",\n".join(f"    {literal(points)}" for points, _ in cases)
    counts = [len(sizes) for _, sizes in cases]
    sizes = [size for _, case_sizes in cases for size in case_sizes]
    out = f"""{HEADER}
{{-
   GraphemeBreakTest.txt: each case's text, how many clusters it has, and how many characters each
   of those clusters holds, all cases' clusters in one run. `Clusters.test.yana` walks them.
-}}

let graphemeBreakTexts = [
{texts}
] :: [String *{len(cases)}]

{array("graphemeBreakCounts", counts, "U8")}
{array("graphemeBreakSizes", sizes, "U8")}"""
    with open(os.path.join(LIBRARY, "GraphemeBreakTest.test.yana"), "w") as f:
        f.write(out)


def block_checksums(values):
    """
    A checksum of each 0x1000 code points' values, which `Tries.test.yana` recomputes through the
    lookups. It rotates and XORs rather than multiplying, so it is exact on JS, where a `U32` product
    is a double.
    """
    sums = []
    for start in range(0, CODE_POINTS, 0x1000):
        h = 0
        for value in values[start:start + 0x1000]:
            h = (((h << 5) | (h >> 27)) & 0xFFFFFFFF) ^ value
        sums.append(h)
    return sums


def write_case_tables(index, entries, strings):
    trie = checked_trie(index)
    size = trie.size + len(entries) * 12 + len(strings) * 4
    columns = list(zip(*entries))
    out = f"""{HEADER}
{{-
   Full case mappings - Implementation-String.md part 8.3, read by `Case.yana`: {size} bytes.

   `caseIndexAt` answers one of {len(entries)} entries, and each entry is three mappings - lowercase,
   uppercase and case folding - each one either the difference from the character to its single
   mapped character, or `caseStringTag + (offset << 2) + length`, a run of up to three characters of
   `caseStrings`. Entry 0 maps every character to itself, and is what all but
   {sum(1 for value in index if value)} code points have.
-}}

let caseStringTag = {CASE_STRING_TAG} :: I32

{array("caseLowerDeltas", columns[0], "I32")}
{array("caseUpperDeltas", columns[1], "I32")}
{array("caseFoldDeltas", columns[2], "I32")}
{array("caseStrings", strings, "U32")}
{trie_source("caseIndex", trie, element_type(len(entries)), 0, "the entry that changes nothing")}"""
    with open(os.path.join(LIBRARY, "CaseTables.yana"), "w") as f:
        f.write(out)
    return trie, size


def write_normalization_tables(index, entries, pool):
    trie = checked_trie(index)
    size = trie.size + len(entries) * 4 + len(pool) * 4
    bits = "\n".join(f"let {constant} = {1 << bit} :: U32" for _, _, bit, constant in NORMALIZATION_FLAGS)
    out = f"""{HEADER}
{{-
   Canonical and compatibility decomposition and composition - Implementation-String.md part 8.4,
   read by `Normalization.yana`: {size} bytes.

   `normalizationIndexAt` answers one of {len(entries)} entries. An entry is the character's
   canonical combining class in the low 8 bits, its quick-check answers in the bits below, and from bit
   {NORMALIZATION_DATA_SHIFT} an offset into `normalizationData`, zero when there is nothing there. At the offset, a
   header word `canonical | compatible << 5 | pairs << 10` counts what follows: the full canonical
   decomposition, the full compatibility decomposition when it differs from the canonical one, and
   the pairs `second, composite` of each primary composite this character starts. Hangul syllables
   are computed and have no data.
-}}

{bits}
let normalizationDataShift = {NORMALIZATION_DATA_SHIFT} :: U32

{array("normalizationEntries", entries, "U32")}
{array("normalizationData", pool, "U32")}
{trie_source("normalizationIndex", trie, element_type(len(entries)), 0, "the entry of a starter with no mappings")}"""
    with open(os.path.join(LIBRARY, "NormalizationTables.yana"), "w") as f:
        f.write(out)
    return trie, size


def write_collation_tables(values, elements, contractions, ascii):
    trie = checked_trie(values)
    size = trie.size + len(elements) * 4 + len(contractions) * 4
    out = f"""{HEADER}
{{-
   The root collation order - Implementation-String.md part 8.5, read by `Collation.yana`: {size} bytes.

   CLDR {CLDR_VERSION}'s `allkeys_CLDR.txt`, which is the DUCET as CLDR's root collation modifies it.
   `collationValueAt` answers a character's one collation element, packed as
   `primary << 16 | secondary << 7 | tertiary << 2 | variable << 1`, or - when the low bit is set -
   `payload << 4 | kind << 1 | 1`:

   - kind 0, **implicit**: the character is not in the table. The payload is the base weight `<< 3`,
     plus one more than the index in `collationSiniformStarts` of the range a siniform script's second
     weight counts from.
   - kind 1, **expansion**: `offset << 5 | count` elements of `collationElements`.
   - kind 2, **contraction**: the index of a record in `collationContractions`. It holds the
     character's own elements as `offset << 5 | count`, how many longer sequences start with it,
     and for each of those, longest first, `extra | count << 2 | offset << 7` followed by the one or two
     code points after the first (zero when there is one).
-}}

let collationSiniformStarts = [{", ".join(f"0x{start:X}" for start in SINIFORM_STARTS)}] :: [U32 *{len(SINIFORM_STARTS)}]

{array("collationElements", elements, "U32")}
{array("collationContractions", contractions, "U32")}
-- Each ASCII character's one element in any all-ASCII text: none of them expands, and the two that
-- start a contraction (`l` and `L`, before a middle dot) never find the rest of it in ASCII.
{array("collationAsciiElements", ascii, "U32")}
{trie_source("collationValue", trie, "U32", COLLATION_OUTSIDE, "an unassigned code point's implicit weights")}"""
    with open(os.path.join(LIBRARY, "CollationTables.yana"), "w") as f:
        f.write(out)
    return trie, size


def text_literal(points, words):
    """A string literal of `points`: graphic characters as themselves, and the rest escaped."""
    graphic = {i for i, (short, _) in enumerate(CATEGORIES) if short[0] in "LMNPS"}
    out = []
    escaped = False
    for c in points:
        category = words[c] & ((1 << CATEGORY_BITS) - 1)
        raw = category in graphic and chr(c) not in '"\\{}' and not (escaped and chr(c) in "0123456789abcdefABCDEF")
        out.append(chr(c) if raw else f"\\x{c:X}")
        escaped = not raw
    return '"' + "".join(out) + '"'


def string_array(name, texts, words):
    body = ",\n".join(f"    {text_literal(points, words)}" for points in texts)
    return f"let {name} = [\n{body}\n] :: [String *{len(texts)}]\n"


def write_tries_test(tries):
    checks = []
    arrays = []
    for name, trie, values, outside in tries:
        arrays.append(array(f"expected{name[0].upper()}{name[1:]}Sums", block_checksums(values), "U32"))
        checks.append(f"""
@test fn {name}EveryCodePoint():
    let &failures = 0 :: Size

    for block in 0 .. ({CODE_POINTS // 0x1000} :: U32):
        let &sum = 0 :: U32
        for offset in 0 .. (0x1000 :: U32):
            let value = {name}At(Char((block * 0x1000) + offset)) :: U32
            sum = (((sum `shl` 5) `or` (sum `shr` 27)) `xor` value)

        if sum != expected{name[0].upper()}{name[1:]}Sums[truncate(block) :: Size]:
            if failures < 8 then check(False, note: "block {{block}}")
            failures += 1

    checkEqual(failures, 0)
    checkEqual({name}At(Char(0x110000)) :: U32, {outside})
""")
    out = f"""{HEADER}
{{-
   The case, normalization and collation tries, checked for every code point.

   The expectation is a checksum of each 0x1000 code points' values rather than the values, which
   would be most of the tables again. A wrong value names its block.
-}}

import Test

{"".join(arrays)}{"".join(checks)}"""
    with open(os.path.join(LIBRARY, "Tries.test.yana"), "w") as f:
        f.write(out)


def write_case_test(lower, upper, fold, casefold, words):
    points = sorted(set(lower) | set(upper) | set(fold))
    keyed = sorted(casefold)
    out = f"""{HEADER}
{{-
   Every code point whose full lowercase, uppercase or case folding is not itself, with the three
   mappings; and every code point NFKC_Casefold changes, with its value. `Case.test.yana` walks them.
-}}

{array("caseTestPoints", points, "U32")}
{string_array("caseTestLowers", [lower.get(c, [c]) for c in points], words)}
{string_array("caseTestUppers", [upper.get(c, [c]) for c in points], words)}
{string_array("caseTestFolds", [fold.get(c, [c]) for c in points], words)}
{array("nfkcCaseFoldPoints", keyed, "U32")}
{string_array("nfkcCaseFoldValues", [casefold[c] for c in keyed], words)}"""
    with open(os.path.join(LIBRARY, "CaseTest.test.yana"), "w") as f:
        f.write(out)


def separator(texts):
    """A private-use character none of `texts` holds, to join several of them into one literal."""
    used = {c for text in texts for c in text}
    return next(c for c in range(0xE000, 0xF900) if c not in used)


def joined(texts, between):
    out = []
    for text in texts:
        if out:
            out.append(between)
        out.extend(text)
    return out


def write_normalization_test(cases, listed, words):
    between = separator([column for case in cases for column in case])
    rows = [joined(case, between) for case in cases]
    out = f"""{HEADER}
{{-
   NormalizationTest.txt: each case's five columns - source, NFC, NFD, NFKC, NFKD - in one literal,
   separated by `normalizationTestSeparator`, which appears in no column; and the code points its
   part 1 lists, since every character that is not listed is its own normalization in all four forms.
   One literal per case rather than per column, because the compiler holds at most 65,535 elements in
   one array literal. `Normalization.test.yana` walks them.
-}}

let normalizationTestSeparator = Char(0x{between:X})

{string_array("normalizationTestCases", rows, words)}
{array("normalizationTestListed", sorted(listed), "U32")}"""
    with open(os.path.join(LIBRARY, "NormalizationTest.test.yana"), "w") as f:
        f.write(out)


# How many lines of the collation test one literal holds.
COLLATION_LINES_PER_LITERAL = 256


def write_collation_test(cases, words):
    between = separator(cases)
    chunks = [joined(cases[start:start + COLLATION_LINES_PER_LITERAL], between)
              for start in range(0, len(cases), COLLATION_LINES_PER_LITERAL)]
    out = f"""{HEADER}
{{-
   CLDR {CLDR_VERSION}'s CollationTest_CLDR_NON_IGNORABLE_SHORT.txt, in its order, without the lines
   holding a surrogate code point (UTF-8 cannot hold one). Each line sorts at or after the one before
   it, and `Collation.test.yana` checks that it does.

   {len(cases)} lines, {COLLATION_LINES_PER_LITERAL} to a literal and separated by `collationTestSeparator`, which is in no
   line: the compiler holds at most 65,535 elements in one array literal.
-}}

let collationTestSeparator = Char(0x{between:X})

{string_array("collationTestChunks", chunks, words)}"""
    with open(os.path.join(LIBRARY, "CollationTest.test.yana"), "w") as f:
        f.write(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    cache = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "yana-ucd", VERSION)
    parser.add_argument("--ucd", default=None, help=f"a directory holding the files (default: fetch into {cache})")
    args = parser.parse_args()

    ucd = args.ucd
    if ucd is None:
        fetch(cache)
        ucd = cache

    check_white_space(ucd)
    words = property_words(ucd)
    classes = cluster_classes(ucd)
    cases = grapheme_cases(ucd)
    data = unicode_data(ucd)
    lower, upper, fold = case_mappings(ucd, data)
    case_index, case_entries, case_strings = case_tables(lower, upper, fold)
    normalization_index, normalization_entries, normalization_pool = normalization_tables(ucd, data)
    normalization_tests, normalization_listed = normalization_cases(ucd)
    collation_values, collation_elements, collation_contractions, collation_ascii = collation_tables(ucd)
    collation_tests, collation_skipped = collation_cases(ucd)

    os.makedirs(LIBRARY, exist_ok=True)
    property_size, break_size = write_tables(words, classes)
    word_runs, class_runs = write_test(words, classes)
    write_grapheme_cases(cases)
    case_trie, case_size = write_case_tables(case_index, case_entries, case_strings)
    normalization_trie, normalization_size = write_normalization_tables(
        normalization_index, normalization_entries, normalization_pool)
    collation_trie, collation_size = write_collation_tables(collation_values, collation_elements,
                                                           collation_contractions, collation_ascii)
    write_tries_test([("caseIndex", case_trie, case_index, 0),
                      ("normalizationIndex", normalization_trie, normalization_index, 0),
                      ("collationValue", collation_trie, collation_values, COLLATION_OUTSIDE)])
    write_case_test(lower, upper, fold, nfkc_casefold(ucd), words)
    write_normalization_test(normalization_tests, normalization_listed, words)
    write_collation_test(collation_tests, words)
    print(f"Unicode {VERSION}: property tables {property_size} bytes, cluster break tables {break_size} "
          f"bytes, {word_runs} + {class_runs} runs and {len(cases)} grapheme cases in the tests")
    print(f"case tables {case_size} bytes, normalization tables {normalization_size} bytes, collation "
          f"tables {collation_size} bytes; {len(normalization_tests)} normalization cases and "
          f"{len(collation_tests)} collation lines ({collation_skipped} with a surrogate left out)")


if __name__ == "__main__":
    main()
