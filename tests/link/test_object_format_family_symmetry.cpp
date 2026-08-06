// D-FORMAT-MACHO-X86_64-STATICLIB-DECLARES-NO-EXTERN-CALL-DISPATCH — the
// DURABLE half. The specific missing key is a one-line config fix; this file is
// the reason the class stops recurring.
//
// ★ THE CLASS. Three `macho64-x86_64`-only declaration gaps landed in three
// consecutive cycles — `supportedDataSections` on the `-exec` file, an
// entry-cluster gap closed in TF-C120, and now `externCallDispatch` on the
// relocatable + `-staticlib` pair. Every one was INVISIBLE UNTIL EXECUTION,
// because nothing in this repository ever compared a shipped format against its
// own siblings. Reactive discovery — wait for a leg to reach the code, read the
// fail-loud, add the key — is a working process that does not CONVERGE: it finds
// exactly one gap per cycle, forever, and only for the tiers someone runs.
//
// ★ WHAT THIS TEST ASSERTS. Group the 24 shipped `.format.json` files into
// FAMILIES by stripping the ARCHITECTURE token from the file name, so a family
// is "the same container, OS and artifact tier on a different CPU"
// (macho64-{arm64,x86_64}-darwin-staticlib is one family; elf64-{aarch64,x86_64}
// -linux-exec is another). Two members of a family should differ only in
// arch-specific VALUES — cputype, reloc rows, page size — never in WHICH AXES
// THEY DECLARE AT ALL. So: every root key declared by one arm and not another
// must be justified, at the site, by a `$absentKeyRationale` entry in the file
// that lacks it.
//
// ⚠ AND CRITICALLY, IT DOES NOT DEMAND SYMMETRY — IT DEMANDS A DECLARED ANSWER.
// TF-C120's design audit MEASURED that several `macho64-x86_64` absences are
// DELIBERATE: `tlsAccess` on the x86_64 exec is PINNED ABSENT by
// `Linker.TdataItemOnNonOptedInFormatsRejectsAtAcceptsGate`, and one nested
// absence (`image.segmentPageSize`) exists because cloning the arm64 sibling's
// 16 KiB value would make the format fail its OWN validator's Mach-O
// mmap-congruence rule. A test that simply erased differences would break
// shipped formats at LOAD. The rationale table is therefore the POINT of the
// design, not an escape hatch bolted onto it: a difference must be a decision
// somebody wrote down, and the two failure directions below make it impossible
// to either skip the writing or let it rot.
//
// ⚠ SCOPE, STATED SO IT IS NOT MISTAKEN FOR MORE: ROOT keys only. Descending
// into nested blocks would demand a rationale for every legitimately
// arch-specific value (`macho.cputype`, `image.segmentPageSize`, every
// `relocations[]` row), which is noise, not signal — and the deliberate nested
// deferrals above are already pinned by their own named tests. All three
// measured instances of this class were ROOT-key absences.
//
// ★ THIS FILE FOUND A FOURTH INSTANCE BEFORE IT WAS EVEN WRITTEN, and that is
// the argument for it: applying the sibling diff by hand surfaced
// `longDoubleFormat` MISSING from `macho64-arm64-darwin-dylib` while its three
// arm64 siblings declare `f64` — so `long double` compiled on the arm64 `.o` and
// `-exec` and was REJECTED on the `.dylib` (S0056) from the same source. The
// class is therefore NOT "macho64-x86_64 was filled in reactively"; it is "no
// format was ever compared to its siblings", and the arm64 family had a hole of
// its own. That key is declared in the same commit as this test.

#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using json = nlohmann::json;

namespace {

constexpr std::string_view kRationaleKey = "$absentKeyRationale";

// The shipped-config tree, through the ONE test-side resolver so an
// OUT-OF-TREE build finds it (the `test_c_symbol_decoration.cpp` precedent).
[[nodiscard]] fs::path objectFormatsDir() {
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return {};
    }
    return *root / "src" / "dss-config" / "object-formats";
}

struct Format {
    std::string  name;      // file stem, e.g. "macho64-x86_64-darwin-staticlib"
    std::string  arch;      // the stripped token, e.g. "x86_64"
    std::string  family;    // stem MINUS the arch token, e.g. "macho64-darwin-staticlib"
    json         doc;
    std::set<std::string> declaredKeys;   // non-`$` root keys
};

[[nodiscard]] std::vector<std::string> splitOnDash(std::string const& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char const c : s) {
        if (c == '-') { out.push_back(cur); cur.clear(); }
        else          { cur.push_back(c); }
    }
    out.push_back(cur);
    return out;
}

// Enumerated FROM DISK, never from a hard-coded list: a format added tomorrow is
// covered the day it lands, which is the only way a guard against
// "nobody compared the new file to its siblings" can work at all.
[[nodiscard]] std::vector<Format> loadShippedFormats() {
    std::vector<Format> formats;
    auto const dir = objectFormatsDir();
    if (dir.empty()) return formats;

    std::vector<fs::path> paths;
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator{dir, ec}) {
        auto const p = entry.path();
        if (p.extension() != ".json") continue;
        if (p.filename().string().find(".format.json") == std::string::npos) continue;
        paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end());

    for (auto const& p : paths) {
        std::ifstream in{p, std::ios::binary};
        if (!in) { ADD_FAILURE() << "cannot open " << p.string(); continue; }
        std::string const text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object()) {
            ADD_FAILURE() << p.filename().string() << " is not a JSON object";
            continue;
        }

        Format f;
        std::string const filename = p.filename().string();
        f.name = filename.substr(0, filename.size() - std::string_view{".format.json"}.size());
        f.doc  = std::move(doc);
        for (auto it = f.doc.begin(); it != f.doc.end(); ++it) {
            if (!it.key().empty() && it.key().front() == '$') continue;
            f.declaredKeys.insert(it.key());
        }

        // `<container><bits>-<arch>-<os>[-<tier>]`. Fewer than three tokens
        // means a single-target format (`wasm32-v1`, `spirv-1.6`) with no arch
        // axis and therefore no siblings — it lands in a one-member family and
        // is skipped below, rather than being silently dropped here.
        auto const toks = splitOnDash(f.name);
        if (toks.size() >= 3) {
            f.arch = toks[1];
            for (std::size_t i = 0; i < toks.size(); ++i) {
                if (i == 1) continue;
                if (!f.family.empty()) f.family += '-';
                f.family += toks[i];
            }
        } else {
            f.arch   = "(none)";
            f.family = f.name;
        }
        formats.push_back(std::move(f));
    }
    return formats;
}

// Root keys this arm's siblings declare and it does not.
[[nodiscard]] std::set<std::string> absentVsSiblings(
        Format const& arm, std::vector<Format const*> const& family) {
    std::set<std::string> absent;
    for (Format const* sib : family) {
        if (sib == &arm) continue;
        for (auto const& k : sib->declaredKeys) {
            if (!arm.declaredKeys.contains(k)) absent.insert(k);
        }
    }
    return absent;
}

[[nodiscard]] std::map<std::string, std::vector<Format const*>>
byFamily(std::vector<Format> const& formats) {
    std::map<std::string, std::vector<Format const*>> fams;
    for (auto const& f : formats) fams[f.family].push_back(&f);
    return fams;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// T0 — the ENUMERATION itself, because every assertion below is a loop over
// it and an empty loop is a silent pass.
//
// This project has anchored the shape twice already (the anchor guard's
// fail-open on a missing root; a smoke gate scoring a dead subject as green).
// A wrong repo root, a renamed directory or a changed file suffix would leave
// every test in this file iterating over nothing and reporting success.
// ─────────────────────────────────────────────────────────────────────────
TEST(ObjectFormatFamilySymmetry, EnumerationSeesEveryShippedFormat) {
    auto const formats = loadShippedFormats();
    // 24 shipped today. The floor catches COLLAPSE (a moved tree, a changed
    // suffix), not drift, so adding a 25th format does not red this.
    EXPECT_GE(formats.size(), 20u)
        << "only " << formats.size() << " shipped formats were enumerated — the "
           "SCAN collapsed, and every assertion in this file would then loop "
           "over nothing and pass. Fix the scan; do not lower the floor.";

    // And the grouping must actually GROUP: if arch-token stripping broke,
    // every format would land in its own one-member family and the whole file
    // would degenerate into a no-op that still says PASS.
    auto const fams = byFamily(formats);
    std::size_t multiArm = 0;
    for (auto const& [_, arms] : fams) if (arms.size() >= 2) ++multiArm;
    EXPECT_GE(multiArm, 5u)
        << "only " << multiArm << " families have two or more arms — the "
           "arch-token stripping is broken, so nothing is being COMPARED "
           "even though the files were read.";
}

// ─────────────────────────────────────────────────────────────────────────
// T1 — DIRECTION ONE: an undeclared asymmetry fails loud.
// A key a sibling declares and this file does not must carry a reason.
// ─────────────────────────────────────────────────────────────────────────
TEST(ObjectFormatFamilySymmetry, EveryKeySetAsymmetryIsJustifiedAtTheSite) {
    auto const formats = loadShippedFormats();
    ASSERT_GE(formats.size(), 20u);
    auto const fams = byFamily(formats);

    for (auto const& [family, arms] : fams) {
        if (arms.size() < 2) continue;   // no sibling to diff against
        for (Format const* arm : arms) {
            auto const absent = absentVsSiblings(*arm, arms);
            if (absent.empty()) continue;

            json const rationale = arm->doc.contains(kRationaleKey)
                                 ? arm->doc.at(kRationaleKey) : json::object();
            EXPECT_TRUE(rationale.is_object())
                << arm->name << ": `" << kRationaleKey << "` must be a JSON object";

            for (auto const& key : absent) {
                std::string declarers;
                for (Format const* sib : arms) {
                    if (sib == arm || !sib->declaredKeys.contains(key)) continue;
                    if (!declarers.empty()) declarers += ", ";
                    declarers += sib->name;
                }
                bool const present = rationale.is_object()
                                  && rationale.contains(key)
                                  && rationale.at(key).is_string()
                                  && !rationale.at(key).get<std::string>().empty();
                EXPECT_TRUE(present)
                    << "UNJUSTIFIED FORMAT-FAMILY ASYMMETRY\n"
                    << "  family : " << family << "\n"
                    << "  file   : " << arm->name << ".format.json\n"
                    << "  key    : '" << key << "' — declared by " << declarers
                    << ", absent here, with no rationale.\n"
                    << "  This is the shape that produced three shipped "
                       "macho64-x86_64 gaps in three cycles, each found only "
                       "when a leg finally executed the code that needed the "
                       "key.\n"
                    << "  Fix EITHER by declaring '" << key << "' here, OR by "
                       "adding a `" << kRationaleKey << "` entry in "
                    << arm->name << ".format.json saying why the absence is "
                       "correct — a deliberate deferral, an arch-specific ABI "
                       "fact, or a named open anchor. Do NOT bulk-copy the "
                       "sibling's value: several absences in this tree are "
                       "load-bearing and one would fail the format's own "
                       "validator.";
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T2 — DIRECTION TWO: a STALE rationale fails loud.
//
// Without this, the table rots the moment a gap is closed: someone adds the
// key, the justification for its absence stays behind, and the file now
// carries a written reason for something that is no longer true. Worse, the
// next reader treats that stale prose as current documentation. A one-way
// check would have let this file's own `externCallDispatch` rationale survive
// the fix that made it false.
// ─────────────────────────────────────────────────────────────────────────
TEST(ObjectFormatFamilySymmetry, NoRationaleOutlivesTheAbsenceItExplains) {
    auto const formats = loadShippedFormats();
    ASSERT_GE(formats.size(), 20u);
    auto const fams = byFamily(formats);

    for (auto const& [family, arms] : fams) {
        for (Format const* arm : arms) {
            if (!arm->doc.contains(kRationaleKey)) continue;
            json const& rationale = arm->doc.at(kRationaleKey);
            ASSERT_TRUE(rationale.is_object())
                << arm->name << ": `" << kRationaleKey << "` must be an object";

            auto const absent = absentVsSiblings(*arm, arms);
            for (auto it = rationale.begin(); it != rationale.end(); ++it) {
                EXPECT_TRUE(absent.contains(it.key()))
                    << "STALE FORMAT-FAMILY RATIONALE\n"
                    << "  file : " << arm->name << ".format.json\n"
                    << "  key  : '" << it.key() << "' has a `" << kRationaleKey
                    << "` entry, but it is NOT absent-vs-siblings — either the "
                       "key is now declared here (the gap closed and the "
                       "excuse outlived it), or NO sibling in family '"
                    << family << "' declares it, so there is no asymmetry to "
                       "explain.\n"
                    << "  Delete the entry. A written reason for something "
                       "that is no longer true is worse than no reason: it "
                       "reads as current documentation.";
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T3 — the specific gap this cycle closed, pinned by NAME so a regression is
// attributable rather than folded into the generic sweep above.
//
// The generic test would catch a re-removal only as "an unjustified
// asymmetry"; this one says which key, on which formats, and asserts the
// VALUE — because a wrong value here is not a build error but a wrong call
// shape (`indirect-slot` in a Mach-O relocatable dereferences the stub's own
// code bytes as a function pointer).
// ─────────────────────────────────────────────────────────────────────────
TEST(ObjectFormatFamilySymmetry, EveryMachoFormatDeclaresDirectPltExternCall) {
    for (char const* name : {"macho64-x86_64-darwin",
                             "macho64-x86_64-darwin-staticlib",
                             "macho64-x86_64-darwin-exec",
                             "macho64-x86_64-darwin-dylib",
                             "macho64-arm64-darwin",
                             "macho64-arm64-darwin-staticlib",
                             "macho64-arm64-darwin-exec",
                             "macho64-arm64-darwin-dylib"}) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name;
        ASSERT_TRUE((*r)->externCallDispatch().has_value())
            << name << " declares no externCallDispatch — every extern call on "
                       "this format fails at MIR→LIR with "
                       "L_RequiredLirOpcodeMissing "
                       "(D-FORMAT-MACHO-X86_64-STATICLIB-DECLARES-NO-EXTERN-CALL-DISPATCH)";
        EXPECT_EQ(*(*r)->externCallDispatch(), ExternCallDispatch::DirectPlt)
            << name << ": Mach-O resolves imports through a per-extern __stubs "
                       "entry and the CALL SITE is a plain direct call to that "
                       "stub — `indirect-slot` is the PE IAT shape and would "
                       "deref the stub's code bytes as a function pointer.";
    }
}

// The fourth instance, found by applying this file's own method by hand before
// the file existed. Pinned by name for the same reason as T3: the generic sweep
// would report only "unjustified asymmetry", not that a `.dylib` and a `.o`
// built from the same TU disagreed about whether `long double` exists.
TEST(ObjectFormatFamilySymmetry, EveryDarwinFormatDeclaresItsLongDoubleAxis) {
    struct Row { char const* name; LongDoubleFormat expected; };
    for (Row const row : {
             Row{"macho64-arm64-darwin",            LongDoubleFormat::F64},
             Row{"macho64-arm64-darwin-exec",       LongDoubleFormat::F64},
             Row{"macho64-arm64-darwin-staticlib",  LongDoubleFormat::F64},
             Row{"macho64-arm64-darwin-dylib",      LongDoubleFormat::F64},
             Row{"macho64-x86_64-darwin",           LongDoubleFormat::X87_80},
             Row{"macho64-x86_64-darwin-exec",      LongDoubleFormat::X87_80},
             Row{"macho64-x86_64-darwin-staticlib", LongDoubleFormat::X87_80},
             Row{"macho64-x86_64-darwin-dylib",     LongDoubleFormat::X87_80}}) {
        auto r = ObjectFormatSchema::loadShipped(row.name);
        ASSERT_TRUE(r.has_value()) << row.name;
        EXPECT_EQ((*r)->longDoubleFormat(), row.expected)
            << row.name << ": a Darwin format that omits the axis REJECTS "
                           "`long double` with S0056 while its siblings compile "
                           "it — same translation unit, different answer.";
    }
}
