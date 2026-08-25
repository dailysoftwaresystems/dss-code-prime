// THE DARWIN LINK-NAME ORACLE — the check that covers EVERY FUTURE symbol.
//
// D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME.
//
// ── WHAT THIS EXISTS TO PREVENT ──────────────────────────────────────────────
//
// Darwin reaches its modern 64-bit-inode ABI through `$INODE64` asm-label
// ALIASES on x86_64 (`sys/cdefs.h`: `__DARWIN_SUF_64_BIT_INO_T` is `"$INODE64"`
// on x86_64 and EMPTY on arm64, where that ABI is the only one). DSS declared
// the PLAIN names while compiling the MODERN 144-byte `struct stat`, so an
// x86_64-Darwin build bound the LEGACY 32-bit-inode implementations: the callee
// writes 120 of 144 bytes, `st_size` is read at offset 96 and written at 72,
// `fstat` hands back 0, sqlite concludes every database file is empty, and the
// shell answers "database disk image is malformed". MEASURED, shipped, and
// SILENT — a descriptor SHADOWS the SDK header entirely
// (`src/core/types/include_path_resolve.hpp`, Descriptor beats Source), so the
// platform's own asm label never participates, and libSystem exports BOTH
// spellings so the plain import links clean and loads clean.
//
// ── WHY IT NEEDS THIS FILE AND NOT ONLY THE FIXED TABLE ──────────────────────
//
// `test_shipped_lib_descriptor.cpp`'s `RealDescriptorsCarryTheirDarwinLinkNames`
// pins the TEN rows that were measured when the fix landed. That is the right
// pin for those ten and it is not enough, because it is a LIST: a symbol added
// to any descriptor tomorrow is covered by nothing. ★ AND THAT IS NOT
// HYPOTHETICAL — that table's own docblock records the fix's sweep as 232
// Mach-O-visible descriptor functions on 2026-08-05; ✔RE-MEASURED 2026-08-25 the
// same sweep is 277. Whatever the exact delta, ~45 symbols entered the
// eager-import set with no one asking the platform what it calls them, and the
// ten-row list could not have noticed.
//
// So this file inverts the question. Instead of "are these ten names right?" it
// asks "is EVERY Mach-O-visible imported function accounted for by a
// MEASUREMENT?" — and a symbol with no measurement goes RED, naming the exact
// command that produces one.
//
// ── THE ONLY CHECK THAT WORKS FOR THIS CLASS ─────────────────────────────────
//
// ⛔ "verify the symbol is exported" does NOT catch it. BOTH `_fstat` AND
// `_fstat$INODE64` exist in libSystem's x86_64 slice, so an export check answers
// YES for the WRONG one — the defect was never a missing name, it was the wrong
// EXISTING name. ⇒ The question that decides a link name is
//
//     "does a REAL compiler for THIS target emit THIS name for THIS C
//      identifier" — compile a one-line TU with the platform toolchain and read
//      the undefined symbol it emits.
//
// That measurement is `tests/ffi/data/darwin-link-names.tsv`, taken on the
// operator's Mac (macOS 26.5.2, Apple `cc`), 236 identifiers x 2 arches. This
// file is the machinery that holds the descriptors to it on every gate, on every
// host — the table is DATA, so the check runs on Windows and Linux where no Mach-O
// can even be executed.
//
// ── THE FOUR AXES, AND WHY EACH IS SEPARATE ──────────────────────────────────
//
//   (1) CENSUS FORWARD — every Mach-O-visible imported function has a measured
//       row. This is the axis that covers the FUTURE.
//   (2) VALUE — the declared `linkName` equals what the oracle implies, on BOTH
//       arches. One arm would be worse than none: a flat alias satisfies x86_64
//       and is WRONG on arm64, which merely relocates the defect.
//   (3) CENSUS BACKWARD — no oracle row outlives its symbol. A stale row is a
//       measurement nothing checks any more, and the next author reads it as
//       current.
//   (4) THE DECORATION IS NEVER IN CONFIG — the oracle stores what `nm` printed
//       (`_fstat$INODE64`), and the expectation is derived by running the
//       FORMAT's own declared rule backwards through `ffi::unapplyCMangling`.
//       Hard-coding "strip one underscore" here would make this file a second
//       owner of a per-format fact, which is the exact shape
//       D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN deleted.
//
// ★ THE VALUE AXIS GOES THROUGH THE REAL READER (`readShippedLibDescriptor` with
// an ACTIVE arch + format), never through a re-implementation of variant
// resolution in the test. A test that resolved `variants` itself would stay
// green if the reader's resolver broke, which is the failure it is here to
// catch.
//
//   (5) THE KEY ITSELF IS REGISTERED in the CLOSED per-symbol key set —
//       unregistered, `linkName` does not get ignored, it makes every
//       descriptor carrying it fail to LOAD.
//
// RED-ON-DISABLE: the closing tests drive the SAME predicates over SYNTHETIC
// inputs, so each guard is exercised even while the real tree is correct
// (the `test_environ_data_object_binding.cpp` /
// `test_pe_crt_costate_binding.cpp` idiom). The real-tree arms were also
// exercised by mutation — see this cycle's report.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/mangling/c_mangle.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// The two Darwin arches DSS ships a Mach-O target for. Kept as data so the
// per-arch loops below never spell an arch twice, and so a third arch is one
// row plus one oracle column rather than an edit in five places.
constexpr std::string_view kArchX86 = "x86_64";
constexpr std::string_view kArchArm = "arm64";

// ── The oracle table ─────────────────────────────────────────────────────────

struct OracleRow {
    std::string probeHeader;  // the header the MEASUREMENT included
    std::string emitted[2];   // [0] = x86_64, [1] = arm64 — DECORATED, as `nm` printed
};

[[nodiscard]] fs::path oraclePath() {
    return dss::test::repoRoot() / "tests" / "ffi" / "data"
           / "darwin-link-names.tsv";
}

// FAIL-CLOSED: a missing file, a short line or a duplicate key all ADD_FAILURE
// rather than yielding a small map that would make every census below vacuous.
[[nodiscard]] std::map<std::string, OracleRow> loadOracle() {
    std::map<std::string, OracleRow> out;
    fs::path const path = oraclePath();
    std::ifstream in(path);
    if (!in) {
        ADD_FAILURE() << "oracle table unreadable: " << path.generic_string()
                      << " — it is the MEASUREMENT this whole file checks "
                         "against; without it nothing below asserts anything";
        return out;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        // A `core.autocrlf` checkout can hand us a trailing CR; strip it here
        // rather than pinning the file's eol in .gitattributes, so the parser is
        // correct no matter how the tree was checked out.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        std::vector<std::string> cells;
        std::size_t start = 0;
        for (;;) {
            std::size_t const tab = line.find('\t', start);
            if (tab == std::string::npos) { cells.push_back(line.substr(start)); break; }
            cells.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (cells.size() != 4) {
            ADD_FAILURE() << path.generic_string() << ":" << lineNo
                          << ": expected 4 tab-separated cells (symbol, "
                             "probeHeader, emitted-x86_64, emitted-arm64), got "
                          << cells.size();
            continue;
        }
        OracleRow row;
        row.probeHeader = cells[1];
        row.emitted[0]  = cells[2];
        row.emitted[1]  = cells[3];
        if (!out.emplace(cells[0], std::move(row)).second) {
            ADD_FAILURE() << path.generic_string() << ":" << lineNo
                          << ": duplicate row for '" << cells[0]
                          << "'. The table is keyed by IDENTIFIER because the "
                             "link name is a property of the platform, not of "
                             "the header that declares it";
        }
    }
    return out;
}

// ── What the oracle IMPLIES the descriptor must declare ──────────────────────
//
// The table stores the DECORATED symbol `nm -u` printed. `linkName` is the
// UNDECORATED base name, and EMPTY when the identifier itself is already right.
// The undecoration runs the FORMAT's declared rule backwards — this file does
// not know that Mach-O uses a leading underscore, it asks `c_mangle` (which is
// itself driven by `cSymbolDecoration.scheme`).
[[nodiscard]] std::string
impliedLinkName(std::string_view symbol, std::string_view emitted) {
    std::string const base =
        unapplyCMangling(emitted, CSymbolDecorationScheme::LeadingUnderscore);
    return base == symbol ? std::string{} : base;
}

// ── Enumerating the Mach-O-visible IMPORTED functions ────────────────────────

struct DescriptorFn {
    std::string descriptor;  // path relative to shippedLibs/, e.g. "sys/stat.json"
    std::string header;      // the descriptor's own `header`
    std::string symbol;      // the C identifier
};

[[nodiscard]] fs::path shippedLibsRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg / "shippedLibs";
}

[[nodiscard]] bool listContains(json const* arr, std::string_view v) {
    if (arr == nullptr || !arr->is_array()) return false;
    for (auto const& e : *arr) {
        if (e.is_string() && e.get<std::string>() == v) return true;
    }
    return false;
}

// The two-level availability fallback the reader applies: the row's own key,
// else the document's, else "every format".
[[nodiscard]] bool availableOnMachO(json const& doc, json const& row) {
    json const* src = nullptr;
    if (row.contains("availableObjectFormats")) src = &row.at("availableObjectFormats");
    else if (doc.contains("availableObjectFormats"))
        src = &doc.at("availableObjectFormats");
    if (src == nullptr) return true;  // no key == every format
    return listContains(src, "macho");
}

// Is this row a Mach-O IMPORT — i.e. a name that will appear in the emitted
// binary's undefined set, and whose spelling therefore has to be right?
//
// THREE exclusions, each for a reason the oracle cannot answer:
//   * `kind: "object"` — a data object, not a call; the `&sym` probe the oracle
//     uses measures functions.
//   * `synthesize` — DSS emits the BODY (the <threads.h> Win32/pthread shims);
//     nothing is imported, so no platform name exists to compare against.
//   * a `macho` `realization` — DSS ships the source for this format
//     (D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF), same reason.
[[nodiscard]] bool isMachOImportedFunction(json const& doc, json const& row) {
    if (!row.is_object() || !row.contains("name") || !row.at("name").is_string())
        return false;
    if (row.contains("kind") && row.at("kind").is_string()
        && row.at("kind").get<std::string>() != "function")
        return false;
    if (row.contains("synthesize")) return false;
    if (!availableOnMachO(doc, row)) return false;
    json const* real = nullptr;
    if (row.contains("realization")) real = &row.at("realization");
    else if (doc.contains("realization")) real = &doc.at("realization");
    if (real != nullptr && real->is_object() && real->contains("macho")) return false;
    return true;
}

// Walk the REAL descriptor tree. FAIL-CLOSED: an unparseable descriptor fails
// here rather than silently shrinking the census.
[[nodiscard]] std::vector<DescriptorFn> machOImportedFunctions() {
    std::vector<DescriptorFn> out;
    fs::path const root = shippedLibsRoot();
    if (root.empty() || !fs::exists(root)) {
        ADD_FAILURE() << "shippedLibs root missing: " << root.generic_string();
        return out;
    }
    std::vector<fs::path> files;
    for (auto const& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file() && e.path().extension() == ".json")
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (auto const& p : files) {
        std::ifstream in(p);
        json doc = json::object();
        try {
            in >> doc;
        } catch (std::exception const& e) {
            ADD_FAILURE() << "shipped descriptor " << p.generic_string()
                          << " does not parse: " << e.what();
            continue;
        }
        if (!doc.contains("header") || !doc.at("header").is_string()) continue;
        if (doc.contains("availableObjectFormats")
            && !listContains(&doc.at("availableObjectFormats"), "macho"))
            continue;
        if (!doc.contains("symbols") || !doc.at("symbols").is_array()) continue;
        for (auto const& row : doc.at("symbols")) {
            if (!isMachOImportedFunction(doc, row)) continue;
            out.push_back(DescriptorFn{
                fs::relative(p, shippedLibsRoot()).generic_string(),
                doc.at("header").get<std::string>(),
                row.at("name").get<std::string>()});
        }
    }
    return out;
}

// c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): the SysV `va_list` named-type binding
// PRODUCTION threads into every shipped-descriptor read — `stdio.json`'s
// `vfprintf` spells `va_list`, and without the binding that signature does not
// decode and the WHOLE descriptor read fails. ✔MEASURED here: omitting it turned
// stdio.json's 37 Mach-O functions into "failed to read", i.e. this census would
// have skipped the largest descriptor in the corpus. Same mint as the analyzer's
// SysVRegisterSave arm; the caller keeps the storage alive across the read.
[[nodiscard]] std::array<NamedTypeBinding, 1>
sysvVaListBinding(TypeInterner& interner) {
    TypeId const voidPtr =
        interner.pointer(interner.primitive(TypeKind::Void));
    std::array<TypeId, 4> tagFields{
        interner.primitive(TypeKind::U32), interner.primitive(TypeKind::U32),
        voidPtr, voidPtr};
    TypeId const vaListTy =
        interner.array(interner.structType("__va_list_tag", tagFields), 1);
    return {NamedTypeBinding{"va_list", vaListTy}};
}

// The `linkName` the REAL READER resolves for one descriptor on one arch. Keyed
// per (descriptor, arch) and cached, because a descriptor read is not free and
// the census walks 277 rows twice.
[[nodiscard]] std::map<std::string, std::string>
readLinkNames(fs::path const& descriptor, std::string_view arch) {
    std::map<std::string, std::string> out;
    TypeInterner       interner{CompilationUnitId{1}};
    TypeRegistry       typeReg;
    DiagnosticReporter rep;
    auto const namedTypes = sysvVaListBinding(interner);
    auto const desc = readShippedLibDescriptor(descriptor, interner, typeReg, rep,
                                               DataModel::Lp64, arch,
                                               ObjectFormatKind::MachO, namedTypes);
    if (!desc.has_value()) {
        ADD_FAILURE() << descriptor.generic_string()
                      << " failed to read for arch " << arch;
        return out;
    }
    for (auto const& s : desc->symbols) out.emplace(s.name, s.linkName);
    return out;
}

}  // namespace

// ── (0) THE TABLE ITSELF ─────────────────────────────────────────────────────

// Fail-closed floor: a table that failed to load, or loaded thin, would make
// every census below pass vacuously.
TEST(DarwinLinkNameOracle, TableLoadsAndIsSubstantial) {
    auto const oracle = loadOracle();
    ASSERT_GE(oracle.size(), 200u)
        << "the measured table holds 236 identifiers; a much smaller one means "
           "it was truncated or the parser stopped early, and every census "
           "below would then pass by measuring nothing";
    // The eight MEASURED divergences, spelled out — the table is data, and this
    // is the assertion that the data still says what the cycle measured.
    struct Expect { char const* sym; char const* x86; char const* arm; };
    constexpr Expect kDiverging[] = {
        {"stat",     "_stat$INODE64",     "_stat"},
        {"fstat",    "_fstat$INODE64",    "_fstat"},
        {"lstat",    "_lstat$INODE64",    "_lstat"},
        {"statfs",   "_statfs$INODE64",   "_statfs"},
        {"fstatfs",  "_fstatfs$INODE64",  "_fstatfs"},
        {"opendir",  "_opendir$INODE64",  "_opendir"},
        {"readdir",  "_readdir$INODE64",  "_readdir"},
        // ★ realpath is the SHAPE A CROSS-ARCH DIFF IS BLIND TO: the same
        // non-plain name on BOTH arches. Its descriptor variant is `format`-keyed
        // where the seven above are (format, arch)-keyed, and a "consistency"
        // edit narrowing it to x86_64 would silently drop the arm64 arm.
        {"realpath", "_realpath$DARWIN_EXTSN", "_realpath$DARWIN_EXTSN"},
    };
    for (auto const& e : kDiverging) {
        auto const it = oracle.find(e.sym);
        ASSERT_NE(it, oracle.end()) << e.sym << " has no measured row";
        EXPECT_EQ(it->second.emitted[0], e.x86) << e.sym << " on x86_64-Darwin";
        EXPECT_EQ(it->second.emitted[1], e.arm) << e.sym << " on arm64-Darwin";
    }
    // And the CONTROLS: names that stay plain on both arches, so a blanket
    // "suffix everything in this header" edit reds here.
    for (char const* plain : {"closedir", "mkdir", "chmod", "fchmod", "open",
                              "close", "read", "write", "fopen", "malloc"}) {
        auto const it = oracle.find(plain);
        ASSERT_NE(it, oracle.end()) << plain << " has no measured row";
        EXPECT_EQ(it->second.emitted[0], std::string("_") + plain)
            << plain << " is MEASURED plain on x86_64-Darwin — the per-SYMBOL "
                        "control proving the rename belongs to individual "
                        "symbols and never to a header";
        EXPECT_EQ(it->second.emitted[1], std::string("_") + plain)
            << plain << " is MEASURED plain on arm64-Darwin";
    }
}

// ── (1) CENSUS FORWARD — the axis that covers every FUTURE symbol ────────────

TEST(DarwinLinkNameOracle, EveryMachOImportedFunctionHasAMeasuredRow) {
    auto const oracle = loadOracle();
    auto const fns    = machOImportedFunctions();
    ASSERT_GE(fns.size(), 250u)
        << "the Mach-O import surface is 277 declared functions; a much smaller "
           "sweep means the enumeration broke and this census is vacuous";
    for (auto const& f : fns) {
        EXPECT_NE(oracle.find(f.symbol), oracle.end())
            << "shippedLibs/" << f.descriptor << " declares `" << f.symbol
            << "` as a Mach-O import, and NOTHING has ever asked a real Darwin "
               "compiler what it calls that identifier.\n"
               "  DSS eager-imports every declared function, and a WRONG-but-"
               "EXISTING spelling links clean, loads clean, and misbinds "
               "silently (see this file's header: that is how `fstat` returned "
               "st_size 0 and sqlite called every database malformed).\n"
               "  MEASURE IT on a real Mac, per arch:\n"
               "    printf '#include <%s>\\nvoid *p(void){return (void*)&%s;}\\n' "
               "<header> " << f.symbol << " > t.c\n"
               "    cc -arch x86_64 -fno-builtin -fno-stack-protector -w -c -o t.o t.c && nm -u t.o\n"
               "    cc -arch arm64  -fno-builtin -fno-stack-protector -w -c -o t.o t.c && nm -u t.o\n"
               "  then add the row to tests/ffi/data/darwin-link-names.tsv. If "
               "the compile fails, the probe header is wrong for the PLATFORM "
               "(DSS's shipped headers are a deliberate superset) — find the "
               "real one and record it in the row; never drop the symbol.";
    }
}

// ── (2) VALUE — both arches, through the REAL reader ─────────────────────────

TEST(DarwinLinkNameOracle, EveryDeclaredLinkNameMatchesTheMeasurement) {
    auto const oracle = loadOracle();
    auto const fns    = machOImportedFunctions();
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty());
    ASSERT_GE(fns.size(), 250u) << "fail-closed: an empty sweep asserts nothing";

    std::string_view const arches[2] = {kArchX86, kArchArm};
    // (descriptor, archIndex) -> symbol -> resolved linkName
    std::map<std::string, std::map<std::string, std::string>> cache;
    int compared = 0;
    for (auto const& f : fns) {
        auto const it = oracle.find(f.symbol);
        if (it == oracle.end()) continue;  // reported by the census test above
        for (int a = 0; a < 2; ++a) {
            std::string const key = f.descriptor + "|" + std::to_string(a);
            auto cit = cache.find(key);
            if (cit == cache.end()) {
                cit = cache.emplace(key, readLinkNames(root / f.descriptor,
                                                       arches[a])).first;
            }
            auto const sit = cit->second.find(f.symbol);
            ASSERT_NE(sit, cit->second.end())
                << f.descriptor << " lost symbol " << f.symbol
                << " when read for " << arches[a];
            std::string const want =
                impliedLinkName(f.symbol, it->second.emitted[a]);
            EXPECT_EQ(sit->second, want)
                << f.symbol << " in shippedLibs/" << f.descriptor << " on "
                << arches[a] << "-Darwin: a real `cc -arch " << arches[a]
                << "` emits `" << it->second.emitted[a] << "` for this "
                   "identifier, so the descriptor must declare linkName `"
                << (want.empty() ? "<absent — the identifier itself>" : want)
                << "` and declares `"
                << (sit->second.empty() ? "<absent>" : sit->second)
                << "`.\n  An EMPTY expectation means the plain name is already "
                   "what the platform binds; a NON-EMPTY one means binding the "
                   "plain name reaches a DIFFERENT callee, silently — it links "
                   "clean either way, which is why only a measurement can "
                   "settle it.";
            ++compared;
        }
    }
    EXPECT_GE(compared, 500)
        << "fail-closed: 277 symbols x 2 arches should be ~554 comparisons; a "
           "small number means the loop skipped nearly everything";
}

// ── (3) CENSUS BACKWARD — no measurement outlives its symbol ─────────────────

TEST(DarwinLinkNameOracle, NoOracleRowIsStale) {
    auto const oracle = loadOracle();
    auto const fns    = machOImportedFunctions();
    ASSERT_GE(fns.size(), 250u) << "fail-closed: an empty sweep asserts nothing";
    std::set<std::string> live;
    for (auto const& f : fns) live.insert(f.symbol);
    for (auto const& [sym, row] : oracle) {
        EXPECT_NE(live.find(sym), live.end())
            << "tests/ffi/data/darwin-link-names.tsv still measures `" << sym
            << "`, which no descriptor declares as a Mach-O import any more. "
               "Delete the row: a measurement nothing checks is read by the "
               "next author as current, and this table's authority comes from "
               "every row in it being live.";
        (void)row;
    }
}

// ── (4) RED-ON-DISABLE — the predicates, exercised on SYNTHETIC inputs ───────
//
// Each arm reproduces one unsafe shape, so the guard is proven to FIRE even
// while the real tree is correct. Without these, "the real tree passes" would be
// equally true of a predicate that always returns true.

TEST(DarwinLinkNameOracle, MissingMeasurementIsCaught) {
    auto oracle = loadOracle();
    ASSERT_NE(oracle.find("fstat"), oracle.end());
    oracle.erase("fstat");
    EXPECT_EQ(oracle.find("fstat"), oracle.end())
        << "the forward census's whole predicate is this lookup — if a removed "
           "row still resolves, the census cannot see a new symbol either";
}

TEST(DarwinLinkNameOracle, WrongSpellingIsCaught) {
    // The EXACT shipped defect: the plain name declared where the platform
    // renames. `impliedLinkName` must disagree with an empty declaration.
    EXPECT_EQ(impliedLinkName("fstat", "_fstat$INODE64"), "fstat$INODE64")
        << "x86_64-Darwin's measured emission implies a NON-EMPTY linkName; if "
           "this returned empty, declaring the plain name would look correct — "
           "which is the state that corrupted a database";
    EXPECT_EQ(impliedLinkName("fstat", "_fstat"), "")
        << "arm64-Darwin's measured emission IS the identifier, so the implied "
           "linkName is absent — the arm a flat alias would break";
    // And the inverse direction, which a one-arch pin cannot see.
    EXPECT_NE(impliedLinkName("fstat", "_fstat"),
              impliedLinkName("fstat", "_fstat$INODE64"))
        << "the two arches must not collapse to one answer";
}

TEST(DarwinLinkNameOracle, ConfigSideDecorationIsCaught) {
    // A descriptor value that already carried Mach-O's `_` would be
    // double-decorated into `__fstat$INODE64`. The derivation strips exactly one
    // decoration, through the format's own rule, so the implied value can never
    // start with `_` unless the IDENTIFIER does.
    EXPECT_FALSE(impliedLinkName("fstat", "_fstat$INODE64").starts_with('_'))
        << "`starts_with`, not `.front()`: the derivation legitimately returns "
           "the EMPTY string on the non-diverging arms, and `front()` on an "
           "empty string is UB that reads as a vacuous pass under a "
           "non-hardened libstdc++";
    // An identifier that genuinely begins with `_` keeps it — the rule strips
    // the FORMAT's decoration, not a character.
    EXPECT_EQ(impliedLinkName("_setjmp", "__setjmp"), "")
        << "one decoration is removed and the identifier is recovered intact";
}

// ── (5) THE KEY ITSELF IS REGISTERED — the LOAD-time property ────────────────
//
// ★ WHY THIS IS ITS OWN TEST AND NOT AN IMPLIED CONSEQUENCE. The per-symbol key
// set is CLOSED: `rejectUnknownKeys` refuses any key not on the list, and the
// refusal fails the WHOLE descriptor read. So an author who adds a descriptor
// key WITHOUT registering it does not get "the key is ignored" — every shipped
// descriptor that carries it stops loading, which under
// D-FFI-DESCRIPTOR-EAGER-IMPORT breaks every `#include` of those headers.
// EIGHT descriptors carry `linkName` today (dirent, io, process, setjmp,
// stdlib, sys/stat, time, unistd), 33 rows between them.
//
// The negative arm is NOT optional: "a document carrying `linkName` reads clean"
// is ALSO true of a reader that accepts every key, which is the failure the
// closed set exists to prevent. So the same document is re-read with a near-miss
// spelling that must still be REFUSED.
TEST(DarwinLinkNameOracle, LinkNameIsRegisteredInTheClosedPerSymbolKeySet) {
    dss::test_support::ScratchDir dir{dss::test_support::Location::Temp,
                                      "darwin-link-name-oracle"};
    auto read = [&](char const* fileName, std::string const& body) -> bool {
        fs::path const p = dir.path() / fileName;
        std::ofstream(p, std::ios::binary) << body;
        TypeInterner       interner{CompilationUnitId{1}};
        TypeRegistry       typeReg;
        DiagnosticReporter rep;
        auto const desc = readShippedLibDescriptor(p, interner, typeReg, rep,
                                                   DataModel::Lp64, kArchX86,
                                                   ObjectFormatKind::MachO);
        return desc.has_value() && !rep.hasErrors();
    };
    constexpr char const* kBody =
        R"JSON({ "header":"sys/stat.h", "library":{"macho":"/usr/lib/libSystem.B.dylib"},
        "symbols":[{ "name":"fstat", "signature":"fn(i32, ptr<void>) -> i32",
            "%KEY%": { "variants": [
                { "when": { "format":"macho", "arch":"x86_64" },
                  "value":"fstat$INODE64" } ] } }] })JSON";
    std::string good{kBody};
    good.replace(good.find("%KEY%"), 5, "linkName");
    std::string typo{kBody};
    typo.replace(typo.find("%KEY%"), 5, "linkname");

    EXPECT_TRUE(read("registered.json", good))
        << "`linkName` must be a REGISTERED member of the per-symbol key set in "
           "`readShippedLibDescriptor`. Unregistered, this read FAILS — and it "
           "fails for every shipped descriptor that carries the key (eight "
           "today), so every binary that #includes one of those headers stops "
           "building. Adding a descriptor key is a two-part edit and this is "
           "the second part.";
    EXPECT_FALSE(read("typo.json", typo))
        << "the negative control: the key set is CLOSED, so a near-miss "
           "spelling must be REFUSED. If this passes, the arm above proves "
           "nothing — a reader that accepts every key would satisfy it, and a "
           "typo'd `linkName` would then silently leave the PLAIN name declared";
}

TEST(DarwinLinkNameOracle, NonFunctionAndSynthesizedRowsAreExcluded) {
    // The three exclusions, each on a synthetic row, so widening the sweep to a
    // shape the oracle cannot measure reds here rather than in a census whose
    // message would blame the missing measurement.
    json const doc = json::parse(R"JSON({ "header": "x.h" })JSON");
    EXPECT_TRUE(isMachOImportedFunction(
        doc, json::parse(R"JSON({"name":"f","signature":"fn() -> i32"})JSON")))
        << "the baseline row IS a Mach-O import — without this, every "
           "EXPECT_FALSE below could pass for the wrong reason";
    EXPECT_FALSE(isMachOImportedFunction(
        doc, json::parse(R"JSON({"name":"e","kind":"object","signature":"i32"})JSON")))
        << "a DATA object has no call to misbind and the `&sym` probe does not "
           "measure it";
    EXPECT_FALSE(isMachOImportedFunction(
        doc, json::parse(R"JSON({"name":"thrd_create","signature":"fn() -> i32",
                                 "synthesize":"thrd_create"})JSON")))
        << "DSS emits the BODY, so nothing is imported and no platform name "
           "exists to compare against";
    EXPECT_FALSE(isMachOImportedFunction(
        doc, json::parse(R"JSON({"name":"g","signature":"fn() -> i32",
                                 "availableObjectFormats":["pe"]})JSON")))
        << "a pe-only row is not on the Mach-O import surface";
    EXPECT_FALSE(isMachOImportedFunction(
        doc, json::parse(R"JSON({"name":"h","signature":"fn() -> i32",
                                 "realization":{"macho":"runtime/x.c"}})JSON")))
        << "DSS ships the source for this format, so it is not imported";
}
