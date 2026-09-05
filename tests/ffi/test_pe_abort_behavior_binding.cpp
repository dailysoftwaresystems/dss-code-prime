// D-FFI-PE-ABORT-BEHAVIOR-NOOP-MACRO-STALE-AFTER-UCRT-FLIP (UCRT-P4) —
// `_set_abort_behavior` must be a REAL ucrtbase.dll binding, never a no-op macro.
//
// WHY THIS TEST EXISTS
//
// While pe's `library.pe` was msvcrt.dll, stdlib.json shipped
// `_set_abort_behavior` as a pe-gated no-op function-like MACRO `(0u)`, and
// that was HONEST: legacy msvcrt exports no such symbol at all, and its
// `abort()` does not raise the UCRT WER fault dialog, so "changes nothing,
// returns old-flags 0" was msvcrt's real semantics. TF-C111 flipped the pe CRT
// to ucrtbase.dll and killed both premises at once — `abort()` now binds UCRT,
// whose abort fast-fails through Windows Error Reporting, and the knob is now
// the SAME runtime's — so the no-op stopped being semantics and became a real
// suppression GAP.
//
// ★ WHAT MAKES THAT GAP WORTH A DEDICATED GUARD IS ITS FAILURE MODE: it is a
// HANG, not a diagnostic. On a host without the ambient WER `DontShowUI`
// policy, an unsuppressed abort can raise a modal dialog and block
// indefinitely. Nothing about that is visible in a compile log, and a plain
// exit-code assertion cannot express it — which is why the run half of this
// fix lives in examples/c/shipped_set_abort_behavior, where the runner
// spawns under `kRunBudget` and a timeout is a hard failure.
//
// WHAT THIS FILE PINS, AND WHY IT IS THE HOST-INDEPENDENT HALF
//
// The run witness needs Windows. The property that actually distinguishes a
// real binding from the `(0u)` macro does NOT: the emitted pe64 image either
// IMPORTS `_set_abort_behavior` FROM `ucrtbase.dll` or it does not, and that is
// a judgment about bytes DSS just wrote. So both pins below stay live on every
// host — Linux, macOS and Windows all build pe64 — exactly like the sibling
// `FfiResolveLibraryRoundTrip.EveryResolvedLibraryReachesTheEmittedDependency-
// Table`. A pin that only the Windows leg can run is a pin four of five legs
// cannot notice regressing.
//
// TWO INDEPENDENT AXES, because the two failure shapes are different:
//
//   1. THE EMITTED IMAGE (`EmittedPe64ImageImportsSetAbortBehaviorFromUcrtbase`)
//      — catches a FULL revert (macro back, symbol row gone): with no symbol row
//      there is nothing to import and the name vanishes from `.idata`.
//   2. THE DESCRIPTOR (`RealStdlibJsonBindsSetAbortBehaviorHonestly`) — catches
//      the PARTIAL revert: restoring ONLY the macro while the symbol row stays,
//      so every CALL SITE silently expands to `(0u)`.
//
//      ⚠⚠ THIS PARAGRAPH USED TO SAY THE IMAGE AXIS WAS BLIND TO THAT MUTATION,
//      AND CARRIED A MEASUREMENT SAYING SO. IT WAS TRUE AND IS NOT ANY MORE. Under
//      the retired eager-import law a declared row was imported whether or not
//      anything called it, so the image still carried `_set_abort_behavior` after
//      the macro came back and axis 1 stayed green. ✔RE-MEASURED 2026-09-03 under
//      referenced-only import ([[D-FFI-DESCRIPTOR-EAGER-IMPORT]]), the mutation
//      applied to a real config tree and the pe64 image walked for its imports:
//          CONTROL (shipped config)   ucrtbase.dll: _set_abort_behavior, exit
//          MUTANT  (macro restored)   ucrtbase.dll: exit
//      The macro shadows every call site, so NOTHING references the row, so the
//      import is DROPPED — and axis 1 reds on the mutation too.
//
//      ⇒ AXIS 2 IS NOT REDUNDANT, and the reason is worth keeping straight: it is
//      the only STATIC check. It refuses the dishonest descriptor without building
//      anything, names WHICH key lies, and holds for symbols no test program in the
//      corpus happens to call — where axis 1 can only report an import that is not
//      there and cannot say why. What is gone is the claim that axis 1 CANNOT see
//      this; do not restore it.
//
// ⚠ NEVER ASSERT AN IMPORT *COUNT*. The pins below ask for a NAME. An aggregate
// count over a table with ~60 entries goes inert the first time an unrelated
// descriptor gains or loses a symbol, and then it is edited reflexively rather
// than read.
//
// ⚠ AND NEVER BIND `_o__set_abort_behavior` (ucrtbase ordinal 1220). It is
// UCRT's internal per-DLL alias, not the API, and it is a decoy that an
// objdump-by-eye search hits first. Its ABSENCE is asserted, so a future
// "the export search found something" edit cannot quietly land on it.
//
// RED-ON-DISABLE. `DescriptorAuditRejectsEveryDishonestShape` runs the REAL
// audit predicate against four synthetic descriptors that each carry exactly one
// dishonest shape (the `(0u)` macro restored; the symbol row deleted; the symbol
// re-widened; a constant given the wrong value), so the audit itself stays
// pinned while the shipped tree is correct. `Pe64ImageWithoutStdlibDoesNotImport-
// SetAbortBehavior` is the negative control for the import matcher: without it,
// a matcher that answered "present" for every input would pass axis 1 forever.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "image_dependency_table.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// ── THE NAMES AND VALUES UNDER TEST ──────────────────────────────────────────
//
// Spelled once, here, because every pin below and the corpus example must agree
// on them. All four MEASURED against the real SDK + the real DLL:
//   * signature verbatim from Windows Kits 10.0.26100.0 ucrt/stdlib.h,
//     `unsigned int __cdecl _set_abort_behavior(unsigned int, unsigned int)`;
//   * the two constants are unsuffixed hex `int` literals at ucrt/stdlib.h
//     — hence `i32` MIRRORING THE SDK even though the parameters are unsigned;
//   * `_set_abort_behavior` is exported by ucrtbase.dll 10.0.26100.8875 at PE
//     ordinal 1771 (objdump's 0-based name-table INDEX 1770 + OrdinalBase 1) and
//     is ABSENT from msvcrt.dll.
constexpr char const* kSymbol       = "_set_abort_behavior";
constexpr char const* kDecoyAlias   = "_o__set_abort_behavior";
constexpr char const* kSignature    = "fn(u32, u32) -> u32";
constexpr char const* kUcrtImage    = "ucrtbase.dll";
constexpr char const* kWriteMsg     = "_WRITE_ABORT_MSG";
constexpr char const* kCallReport   = "_CALL_REPORTFAULT";
constexpr std::int64_t kWriteMsgValue   = 1;
constexpr std::int64_t kCallReportValue = 2;

[[nodiscard]] fs::path stdlibDescriptorPath() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg / "shippedLibs" / "stdlib.json";
}

// ── AXIS 1: THE IMPORTED SYMBOL NAMES OF ONE PE IMAGE ────────────────────────
//
// `tests/test_support/image_dependency_table.hpp` recovers the imported LIBRARY
// names; this recovers the imported SYMBOL names under one library, which is the
// granularity this defect lives at. Same pointer chain, one level deeper:
// e_lfanew → optional header → data directory #1 → each 20-byte
// IMAGE_IMPORT_DESCRIPTOR → its OriginalFirstThunk (the hint/name table) → each
// 8-byte thunk → the 2-byte hint + the NUL-terminated name.
//
// Deliberately NOT a byte scan of `.idata` for the string. A scan cannot tell a
// genuine import row from an incidental string in a neighbouring blob, so it
// would answer "present" for an image that merely mentions the name — which is
// the one answer that must never be wrong here.
//
// Returns EMPTY on any buffer it cannot parse, which is safe because every
// assertion below asks for PRESENCE of a specific name: a reader that silently
// stopped working reds the pin instead of passing vacuously. The negative
// control asserts ABSENCE, so it does NOT rest on this convention alone — it
// first asserts the same image imports a name it certainly does.
[[nodiscard]] std::vector<std::string> peImportedSymbolsFrom(
    std::vector<std::uint8_t> const& b, std::string_view library) {
    using namespace dss::test_support::image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 0x40) return out;
    std::size_t const peOff = rdU32(b, 0x3C);
    if (peOff + 24 > b.size() || rdU32(b, peOff) != 0x00004550u) return out;
    std::size_t   const coffOff = peOff + 4;
    std::uint16_t const numSecs = rdU16(b, coffOff + 2);
    std::uint16_t const optSize = rdU16(b, coffOff + 16);
    std::size_t   const optOff  = coffOff + 20;
    if (rdU16(b, optOff) != 0x020Bu) return out;   // PE32+ magic
    std::uint32_t const importRva = rdU32(b, optOff + 112 + 1 * 8);
    if (importRva == 0) return out;

    std::size_t const secTabOff = optOff + optSize;
    auto rvaToFile = [&](std::uint32_t rva) -> std::size_t {
        for (std::size_t i = 0; i < numSecs; ++i) {
            std::size_t   const s   = secTabOff + i * 40;
            std::uint32_t const va  = rdU32(b, s + 12);
            std::uint32_t const vsz = rdU32(b, s + 8);
            std::uint32_t const rsz = rdU32(b, s + 16);
            std::uint32_t const raw = rdU32(b, s + 20);
            std::uint32_t const span = std::max(vsz, rsz);
            if (span != 0 && rva >= va && rva < va + span)
                return static_cast<std::size_t>(raw) + (rva - va);
        }
        return 0;
    };

    std::size_t desc = rvaToFile(importRva);
    if (desc == 0) return out;
    for (; desc + 20 <= b.size(); desc += 20) {
        std::uint32_t const lookupRva = rdU32(b, desc + 0);   // OriginalFirstThunk
        std::uint32_t const nameRva   = rdU32(b, desc + 12);
        std::uint32_t const firstRva  = rdU32(b, desc + 16);  // FirstThunk
        if (lookupRva == 0 && nameRva == 0 && firstRva == 0) break;  // terminator
        std::size_t const nameOff = rvaToFile(nameRva);
        if (nameOff == 0) break;
        if (rdCStr(b, nameOff) != library) continue;
        // Prefer the import LOOKUP table; a writer may emit only FirstThunk.
        std::uint32_t const thunkRva = lookupRva != 0 ? lookupRva : firstRva;
        std::size_t thunk = rvaToFile(thunkRva);
        if (thunk == 0) continue;
        for (; thunk + 8 <= b.size(); thunk += 8) {
            std::uint64_t const entry = rdU64(b, thunk);
            if (entry == 0) break;                              // end of this DLL
            if ((entry >> 63) != 0) continue;                    // import BY ORDINAL
            std::size_t const hintOff = rvaToFile(
                static_cast<std::uint32_t>(entry & 0x7FFFFFFFu));
            if (hintOff == 0) break;
            out.push_back(rdCStr(b, hintOff + 2));               // skip the 2-byte hint
        }
    }
    return out;
}

[[nodiscard]] bool importsName(std::vector<std::string> const& names,
                               std::string_view needle) {
    return std::find(names.begin(), names.end(), needle) != names.end();
}

[[nodiscard]] std::string joinNames(std::vector<std::string> const& names) {
    std::string s;
    for (auto const& n : names) {
        if (!s.empty()) s += ", ";
        s += n;
    }
    return s;
}

[[nodiscard]] std::vector<std::uint8_t> readWholeBinary(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

// Compile one c source to one target through the real production driver.
[[nodiscard]] int buildOne(fs::path const& outDir, fs::path const& src,
                           std::string const& target, DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    return p.compileFiles(std::vector<std::string>{src.string()}, "c",
                          std::vector<std::string>{target}, rep);
}

[[nodiscard]] fs::path writeSrc(fs::path const& dir, std::string_view name,
                                std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

// ── AXIS 2: THE DESCRIPTOR AUDIT ─────────────────────────────────────────────
//
// ONE predicate, run by the real-tree pin AND by every red-on-disable, so a
// demonstration can never drift from the thing it demonstrates. Returns a
// finding per dishonest shape; EMPTY means the descriptor binds
// `_set_abort_behavior` honestly.
//
// Reads the raw JSON rather than a decoded descriptor because two of the four
// shapes are only visible there: a `macros` entry does not survive
// `readShippedLibDescriptor`'s typed surface, and the pe availability GATE is
// the thing under test (decoding with pe active would hide a row that had
// silently become elf-only). The decoded-type check is a separate pin that runs
// on top of this one.
// A stable empty array for `arrayOf` to hand back on a miss.
//
// ⚠ THIS IS NOT COSMETIC. `arrayOf` originally ended `: nlohmann::json::array();`
// with a DEDUCED return type, so it returned the surface BY VALUE and every
// `named(arrayOf(...), ...)` below produced a pointer into a copy already
// destroyed at the end of the full expression. MEASURED: it did not crash — it
// read back empty strings and -1 values, so the audit reported 8 findings
// against a descriptor that was completely correct. Two things caught it, and
// both are worth keeping: the honest-baseline ASSERT at the top of
// `DescriptorAuditRejectsEveryDishonestShape`, and the fact that the real tree
// and the synthetic baseline failed IDENTICALLY, which pointed at the predicate
// rather than the data. Same class as
// D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE (tests/CMakeLists.txt).
nlohmann::json const kNoRows = nlohmann::json::array();

[[nodiscard]] std::vector<std::string> auditAbortBehaviorBinding(
    nlohmann::json const& doc) {
    std::vector<std::string> findings;
    auto const arrayOf = [&doc](char const* key) -> nlohmann::json const& {
        auto const it = doc.find(key);
        if (it != doc.end() && it->is_array()) return *it;
        return kNoRows;
    };
    auto const named = [](nlohmann::json const& arr, char const* name)
        -> nlohmann::json const* {
        for (auto const& e : arr) {
            if (!e.is_object()) continue;
            auto const n = e.find("name");
            if (n != e.end() && n->is_string() && n->get<std::string>() == name)
                return &e;
        }
        return nullptr;
    };
    // Every pe-gating shape this file accepts: a flat `availableObjectFormats`
    // containing "pe", or a `variants` array whose `when.format` is "pe".
    auto const gatesPeOnly = [](nlohmann::json const& row) {
        auto const av = row.find("availableObjectFormats");
        if (av != row.end() && av->is_array())
            return av->size() == 1 && av->front().is_string()
                && av->front().get<std::string>() == "pe";
        auto const va = row.find("variants");
        if (va == row.end() || !va->is_array() || va->size() != 1) return false;
        auto const& v = va->front();
        if (!v.is_object()) return false;
        auto const w = v.find("when");
        if (w == v.end() || !w->is_object()) return false;
        auto const f = w->find("format");
        return f != w->end() && f->is_string() && f->get<std::string>() == "pe";
    };

    // (a) THE DISHONEST MACRO MUST BE GONE. This is the finding the emitted-image
    //     axis structurally cannot produce, so it has to be produced here.
    if (named(arrayOf("macros"), kSymbol) != nullptr) {
        findings.emplace_back(
            std::string{"`macros` still declares "} + kSymbol
            + " — a macro SHADOWS the symbol row at every call site, so the "
              "declared binding becomes unreachable and every call expands "
              "to a no-op. The no-op was honest only while library.pe was "
              "msvcrt.dll; under UCRT it is a silent WER-suppression gap whose "
              "failure mode is a HANG.");
    }

    // (b) THE SYMBOL ROW MUST EXIST, be pe-only, and keep the SDK's widths.
    auto const* sym = named(arrayOf("symbols"), kSymbol);
    if (sym == nullptr) {
        findings.emplace_back(
            std::string{"`symbols` declares no "} + kSymbol
            + " — with no row the name is undeclared, every caller fails loud, "
              "and nothing is imported.");
    } else {
        auto const sig = sym->find("signature");
        std::string const sigText =
            (sig != sym->end() && sig->is_string()) ? sig->get<std::string>()
                                                    : std::string{};
        if (sigText != kSignature) {
            findings.emplace_back(
                std::string{kSymbol} + " signature is '" + sigText
                + "', expected '" + kSignature
                + "' — the SDK takes and returns `unsigned int`; a narrowed or "
                  "widened arm would pass garbage flags to a stateful CRT knob "
                  "without any diagnostic.");
        }
        if (!gatesPeOnly(*sym)) {
            findings.emplace_back(
                std::string{kSymbol}
                + " is not gated availableObjectFormats [\"pe\"] — this is a "
                  "pe/UCRT spelling with no elf or macho export, so widening the "
                  "gate breaks the LOAD of every binary that REFERENCES it on that "
                  "format (every one that merely included <stdlib.h>, before "
                  "referenced-only import).");
        }
    }

    // (c) BOTH CONSTANTS MUST EXIST, pe-gated, with the SDK's values. They are
    //     what the no-op macro's discarded `mask` argument used to hide.
    struct Expected { char const* name; std::int64_t value; };
    for (auto const& want : {Expected{kWriteMsg, kWriteMsgValue},
                             Expected{kCallReport, kCallReportValue}}) {
        auto const* row = named(arrayOf("constants"), want.name);
        if (row == nullptr) {
            findings.emplace_back(
                std::string{"`constants` declares no "} + want.name
                + " — callers spell the mask with it (there is no "
                  "_ABORT_BEHAVIOR_MASK-style all-bits macro in the SDK).");
            continue;
        }
        if (!gatesPeOnly(*row)) {
            findings.emplace_back(std::string{want.name}
                                  + " is not gated to format pe.");
        }
        // The value + type live inside the single pe variant.
        auto const va = row->find("variants");
        nlohmann::json const& body =
            (va != row->end() && va->is_array() && va->size() == 1)
                ? va->front() : *row;
        auto const val = body.find("value");
        std::int64_t const got =
            (val != body.end() && val->is_number_integer())
                ? val->get<std::int64_t>() : -1;
        if (got != want.value) {
            findings.emplace_back(
                std::string{want.name} + " = " + std::to_string(got)
                + ", expected " + std::to_string(want.value)
                + " (MEASURED against ucrt/stdlib.h by five independent "
                  "instruments, byte-identical across all four installed SDKs).");
        }
        auto const ty = body.find("type");
        std::string const tyText = (ty != body.end() && ty->is_string())
                                       ? ty->get<std::string>() : std::string{};
        if (tyText != "i32") {
            findings.emplace_back(
                std::string{want.name} + " type is '" + tyText
                + "', expected 'i32' — the SDK spells these as unsuffixed hex "
                  "`int` literals, and the descriptor MIRRORS the SDK even though "
                  "the function's parameters are `unsigned int`.");
        }
    }
    return findings;
}

[[nodiscard]] std::string renderFindings(std::vector<std::string> const& f) {
    std::string s;
    for (auto const& e : f) s += "\n  * " + e;
    return s;
}

[[nodiscard]] nlohmann::json readJson(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) return nlohmann::json{};
    return nlohmann::json::parse(in, nullptr, false);
}

// The honest shipped shape, in the SMALLEST descriptor that carries it. Each
// red-on-disable mutates exactly ONE thing about this and expects a finding, so
// this baseline must itself audit CLEAN — asserted before any mutation runs.
[[nodiscard]] nlohmann::json honestDescriptor() {
    return nlohmann::json{
        {"header", "stdlib.h"},
        {"library", {{"pe", kUcrtImage}}},
        {"constants", nlohmann::json::array({
            {{"name", kWriteMsg},
             {"variants", nlohmann::json::array({
                 {{"when", {{"format", "pe"}}}, {"value", kWriteMsgValue},
                  {"type", "i32"}}})}},
            {{"name", kCallReport},
             {"variants", nlohmann::json::array({
                 {{"when", {{"format", "pe"}}}, {"value", kCallReportValue},
                  {"type", "i32"}}})}},
        })},
        {"symbols", nlohmann::json::array({
            {{"name", kSymbol},
             {"signature", kSignature},
             {"kind", "function"},
             {"linkage", "external"},
             {"availableObjectFormats", nlohmann::json::array({"pe"})}},
        })},
    };
}

}  // namespace

// ── AXIS 1 — the emitted image, live on EVERY host ───────────────────────────

// THE HOST-INDEPENDENT DISCRIMINATOR. Nothing is RUN: "does the image import
// this name from this DLL?" is a judgment about bytes DSS just wrote, so Linux
// and macOS legs answer it too.
TEST(PeAbortBehaviorBinding, EmittedPe64ImageImportsSetAbortBehaviorFromUcrtbase) {
    ScratchDir scratch{Location::InsideRepo, "pe-abort-behavior"};
    auto const dir = scratch.path();
    // The call shape test1.c uses: the `mask` argument is the i32 constant, with
    // NO cast. If that implicit conversion ever stopped compiling, the real
    // consumer would break and this build is where it surfaces.
    auto const src = writeSrc(dir, "abortknob.c",
        "#include <stdlib.h>\n"
        "int main(void){ return (int)_set_abort_behavior(0u, _CALL_REPORTFAULT); }\n");

    DiagnosticReporter rep;
    ASSERT_EQ(buildOne(dir, src, "x86_64:pe64-x86_64-windows-exec", rep), 0)
        << "the pe64 build must succeed — a missing symbol row or missing "
           "constant fails loud HERE, which is the honest outcome and also why "
           "this assertion comes first. "
        << (rep.all().empty() ? std::string{} : rep.all().front().actual);
    auto const exePath = dir / "abortknob.exe";
    ASSERT_TRUE(fs::exists(exePath)) << exePath.generic_string();

    auto const bytes = readWholeBinary(exePath);
    ASSERT_FALSE(bytes.empty());

    // The library row must be there before asking what it imports, so a reader
    // that lost the descriptor chain cannot read as "no such symbol".
    auto const libs = dss::test_support::peImportedLibraries(bytes);
    ASSERT_EQ(dss::test_support::dependencyOccurrences(libs, kUcrtImage), 1u)
        << "expected exactly one " << kUcrtImage << " import descriptor; got ["
        << dss::test_support::joinDependencies(libs) << "]";

    auto const names = peImportedSymbolsFrom(bytes, kUcrtImage);
    ASSERT_FALSE(names.empty())
        << "recovered NO imported symbol names from " << kUcrtImage
        << " — the hint/name-table walk is broken, so an absence assertion "
           "below would be vacuous";

    EXPECT_TRUE(importsName(names, kSymbol))
        << kSymbol << " is NOT imported from " << kUcrtImage
        << ". That is exactly the pre-fix state: the no-op `(0u)` macro needs no "
           "import, so its absence here IS the dishonesty. Imported names: ["
        << joinNames(names) << "]";

    EXPECT_FALSE(importsName(names, kDecoyAlias))
        << kDecoyAlias << " must NEVER be bound — it is UCRT's internal "
           "per-DLL alias (ordinal 1220), not the API, and it is the decoy an "
           "objdump search hits first.";
}

// THE NEGATIVE CONTROL for the matcher above. Without it, a `peImportedSymbols-
// From` that answered "present" for any input would keep axis 1 green forever.
// It also pins the AVAILABILITY behaviour that keeps a per-header symbol OUT of an
// image that never asked for it: the name reaches the import table because
// <stdlib.h> was included, not because every pe image gets it. ⓘ Since P57 that is
// the WEAKER of the two gates the name passes — [[D-FFI-DESCRIPTOR-EAGER-IMPORT]]
// made imports referenced-only, so a symbol now needs BOTH its header and a
// reference. This control asserts the header half, which is the half a descriptor
// edit can break.
TEST(PeAbortBehaviorBinding, Pe64ImageWithoutStdlibDoesNotImportSetAbortBehavior) {
    ScratchDir scratch{Location::InsideRepo, "pe-abort-behavior"};
    auto const dir = scratch.path();
    auto const src = writeSrc(dir, "noknob.c",
        "#include <stdio.h>\n"
        "int main(void){ return puts(\"x\") >= 0 ? 0 : 1; }\n");

    DiagnosticReporter rep;
    ASSERT_EQ(buildOne(dir, src, "x86_64:pe64-x86_64-windows-exec", rep), 0)
        << (rep.all().empty() ? std::string{} : rep.all().front().actual);
    auto const bytes = readWholeBinary(dir / "noknob.exe");
    ASSERT_FALSE(bytes.empty());

    auto const names = peImportedSymbolsFrom(bytes, kUcrtImage);
    // The control's OWN precondition: the matcher must be finding real names in
    // THIS image, or its "absent" answer means nothing.
    ASSERT_TRUE(importsName(names, "puts"))
        << "the matcher found no `puts` in an image that certainly imports it, "
           "so its verdict on " << kSymbol << " carries no information. Got: ["
        << joinNames(names) << "]";
    EXPECT_FALSE(importsName(names, kSymbol))
        << kSymbol << " reached the import table of a TU that never included "
           "<stdlib.h> — the per-header availability gate is not holding, and "
           "the matcher above cannot discriminate. Got: [" << joinNames(names)
        << "]";
}

// ── AXIS 2 — the descriptor, the partial-revert axis ─────────────────────────

TEST(PeAbortBehaviorBinding, RealStdlibJsonBindsSetAbortBehaviorHonestly) {
    fs::path const path = stdlibDescriptorPath();
    ASSERT_FALSE(path.empty());
    ASSERT_TRUE(fs::exists(path)) << path.generic_string();

    nlohmann::json const doc = readJson(path);
    ASSERT_FALSE(doc.is_discarded())
        << path.generic_string() << " is not parseable JSON — a malformed "
           "stdlib.json breaks every #include <stdlib.h> on every target";

    auto const findings = auditAbortBehaviorBinding(doc);
    EXPECT_TRUE(findings.empty())
        << findings.size() << " dishonest-binding finding(s) in "
        << path.generic_string() << ':' << renderFindings(findings);
}

// The decoded TYPE, structurally — the widths the SDK declares, inspected
// through the interner rather than compared as text. The audit above pins the
// signature STRING; this pins what the reader actually builds from it, so a
// change to `parseTypeFromText` that silently altered the meaning of that text
// cannot slip through both.
TEST(PeAbortBehaviorBinding, DecodedSignatureIsTwoU32ParamsReturningU32) {
    fs::path const path = stdlibDescriptorPath();
    ASSERT_FALSE(path.empty());

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                         DataModel::Llp64, "x86_64",
                                         ObjectFormatKind::Pe);
    ASSERT_TRUE(desc.has_value())
        << "the REAL stdlib.json must decode for a pe/LLP64 target";
    EXPECT_FALSE(rep.hasErrors());

    auto const it = std::find_if(desc->symbols.begin(), desc->symbols.end(),
                                 [](ShippedSymbol const& s) {
                                     return s.name == kSymbol;
                                 });
    ASSERT_NE(it, desc->symbols.end())
        << kSymbol << " is not in the decoded pe symbol surface";
    EXPECT_EQ(it->kind, ShippedSymbolKind::Function);
    EXPECT_EQ(it->linkage, ShippedSymbolLinkage::External);

    ASSERT_TRUE(it->signature.valid());
    ASSERT_EQ(interner.kind(it->signature), TypeKind::FnSig);
    EXPECT_EQ(interner.kind(interner.fnResult(it->signature)), TypeKind::U32)
        << "the SDK returns `unsigned int` — the PREVIOUS flags word, which the "
           "corpus example reads back through a mask=0 query";
    auto const params = interner.fnParams(it->signature);
    ASSERT_EQ(params.size(), std::size_t{2})
        << "the SDK takes (flags, mask); a one-parameter arm would silently "
           "drop the mask, which is exactly what the no-op macro did";
    EXPECT_EQ(interner.kind(params[0]), TypeKind::U32);
    EXPECT_EQ(interner.kind(params[1]), TypeKind::U32);

    // The two constants must be present on pe with the SDK's values, decoded.
    for (auto const& want : {std::pair{std::string{kWriteMsg}, kWriteMsgValue},
                             std::pair{std::string{kCallReport},
                                       kCallReportValue}}) {
        auto const c = std::find_if(desc->constants.begin(),
                                    desc->constants.end(),
                                    [&](ShippedConstant const& k) {
                                        return k.name == want.first;
                                    });
        ASSERT_NE(c, desc->constants.end())
            << want.first << " is not in the decoded pe constant surface";
        EXPECT_EQ(c->value, want.second);
        EXPECT_EQ(interner.kind(c->type), TypeKind::I32)
            << want.first << " must decode to i32, mirroring the SDK's `int`";
    }
}

// ── RED-ON-DISABLE for axis 2 ────────────────────────────────────────────────
//
// The audit is exercised against the shapes it exists to reject, one mutation
// per case, against a baseline first proven clean. A predicate that always
// returned "no findings" would pass the real-tree pin above; only this can tell
// the difference.
TEST(PeAbortBehaviorBinding, DescriptorAuditRejectsEveryDishonestShape) {
    // The baseline must audit CLEAN, or every "mutation caused a finding" below
    // could be the baseline's own fault.
    ASSERT_TRUE(auditAbortBehaviorBinding(honestDescriptor()).empty())
        << renderFindings(auditAbortBehaviorBinding(honestDescriptor()));

    struct Mutation {
        char const* label;
        char const* mustMention;
        nlohmann::json (*apply)(nlohmann::json);
    };
    Mutation const mutations[] = {
        // THE PARTIAL REVERT — the shape the emitted-image axis cannot see.
        {"the (0u) no-op macro restored alongside the symbol row", "macros",
         [](nlohmann::json d) {
             d["macros"] = nlohmann::json::array({
                 {{"name", kSymbol},
                  {"variants", nlohmann::json::array({
                      {{"when", {{"format", "pe"}}},
                       {"params", nlohmann::json::array({"flags", "mask"})},
                       {"replacement", "(0u)"}}})}}});
             return d;
         }},
        // THE FULL REVERT's descriptor half.
        {"the symbol row deleted", "declares no",
         [](nlohmann::json d) {
             d["symbols"] = nlohmann::json::array();
             return d;
         }},
        // A WIDTH change: silent garbage into a stateful CRT knob.
        {"the signature narrowed to one parameter", "signature is",
         [](nlohmann::json d) {
             d["symbols"][0]["signature"] = "fn(u32) -> u32";
             return d;
         }},
        // A VALUE change: the bit the caller clears is no longer the WER bit.
        {"_CALL_REPORTFAULT given the wrong value", "expected 2",
         [](nlohmann::json d) {
             d["constants"][1]["variants"][0]["value"] = 4;
             return d;
         }},
        // A GATE widened: an import on a format with no such export ⇒ the LOAD
        // of every binary that REFERENCES it breaks (0xC0000139 / exit 127).
        {"the symbol gate widened past pe", "availableObjectFormats",
         [](nlohmann::json d) {
             d["symbols"][0]["availableObjectFormats"] =
                 nlohmann::json::array({"pe", "elf"});
             return d;
         }},
    };

    for (auto const& m : mutations) {
        SCOPED_TRACE(m.label);
        nlohmann::json const mutant = m.apply(honestDescriptor());

        // FAIL-CLOSED #1 — the mutant must DIFFER from the subject, compared as
        // serialized bytes. A mutation that silently applied to nothing (a
        // renamed key, an out-of-range index) would otherwise "pass" by leaving
        // the honest shape in place, and a line count cannot see a same-length
        // replacement.
        ASSERT_NE(mutant.dump(), honestDescriptor().dump())
            << "the mutation changed nothing — it is not testing what it claims";

        // FAIL-CLOSED #2 — the mutant must still be a well-formed descriptor
        // document, so the audit is rejecting the SHAPE rather than tripping
        // over unparseable input.
        ASSERT_TRUE(nlohmann::json::accept(mutant.dump()))
            << "the mutant is not valid JSON; the audit would be rejecting the "
               "wrong thing";

        // FAIL-CLOSED #3 — the SAME predicate the real-tree pin uses must
        // produce a finding, and #4 it must NAME the thing that changed rather
        // than merely counting.
        auto const findings = auditAbortBehaviorBinding(mutant);
        ASSERT_FALSE(findings.empty())
            << "the audit accepted a dishonest descriptor: " << m.label;
        bool named = false;
        for (auto const& f : findings)
            if (f.find(m.mustMention) != std::string::npos) named = true;
        EXPECT_TRUE(named)
            << "the finding must name what is wrong (expected it to mention '"
            << m.mustMention << "'); got:" << renderFindings(findings);
    }
}
