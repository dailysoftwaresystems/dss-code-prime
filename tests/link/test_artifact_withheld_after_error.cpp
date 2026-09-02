// THE ARTIFACT INVARIANT, AT ITS CHOKEPOINT: a compilation that recorded an
// error commits no artifact.
//
// ★★★ WHY THE PIN IS HERE AND NOT ONLY END-TO-END. `linker::writeBytes` is the
// ONE place any artifact byte in this compiler reaches disk — `writeImage`
// delegates its byte commit to it after the LinkedImage-shape preconditions,
// and the `ar` static-archive producer calls it directly; ✔MEASURED, those are
// the only two call sites in `src/`. Enforcing the invariant there is what
// makes it hold for every producer that exists and every one added later. A
// pin that only drove the CLI would prove the pipeline's CURRENT tiers stop in
// time, which is precisely what the defect showed is not enough: the merge
// tier reported an error and let everything downstream run, and the artifact
// was committed by a writer that had been TOLD (in `writeImage`'s own
// K_ImageNotOk message) to check `reporter.errorCount()` and had no way to.
//
// ★★ SEVERITY IS PART OF THE CONTRACT, NOT A DETAIL. The refusal reports
// `K_ArtifactWithheldAfterError` at INFO, following `D_LaterPhasesNotRun`'s
// explicit precedent: it is emitted AT a gate that reads `hasErrors()`, so an
// Error-severity notice would count a second failure for one defect and could
// itself trip the diagnostic cap. `TheNoticeDoesNotRaiseTheErrorCount` pins
// that, because a later "make it louder" edit would silently change what
// `errorCount()` means to every caller downstream of a withheld write.
//
// ★★ AND THE REFUSAL IS NON-DESTRUCTIVE. `AWithheldWriteLeavesAnExistingFile
// ByteIdentical` is the other half of the decision recorded at the fix site:
// gcc 13.3.0, clang 18.1.3 and MSVC 19.51 each UNLINK a previous output when
// `ld`/`link` fails and each LEAVE it byte-identical when the FRONT END fails
// (✔measured separately per reference), so there is no unanimous answer keyed
// on anything but which stage failed. DSS takes the non-destructive half
// everywhere — it never destroys an artifact it did not write. That is a
// deliberate divergence and it is pinned so a later cycle cannot "restore
// parity" without meeting this case.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/linker.hpp"
#include "link/writer.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

constexpr std::uint8_t kPayload[] = {0xDE, 0xAD, 0xBE, 0xEF};

[[nodiscard]] std::span<std::uint8_t const> payload() {
    return std::span<std::uint8_t const>{kPayload, sizeof(kPayload)};
}

// A well-formed image, so nothing but the invariant can refuse the write.
[[nodiscard]] LinkedImage makeImage() {
    LinkedImage img;
    img.format            = ObjectFormatKind::Elf;
    img.bytes             = {0x7F, 'E', 'L', 'F', 0x02, 0x01, 0x01, 0x00};
    img.expectedFuncCount = 1;
    img.resolvedFuncCount = 1;
    img.linkedCleanly     = true;
    return img;
}

// Put ONE error on the reporter, standing in for whatever tier reported it.
// The code is deliberately an ordinary front-end one: the gate reads the
// COUNT, never the identity of what failed, and a gate keyed on a particular
// diagnostic would be the special case this invariant exists to replace.
void recordOneUnrelatedError(DiagnosticReporter& rep) {
    dss::report(rep, DiagnosticCode::S_UndeclaredIdentifier,
                DiagnosticSeverity::Error,
                "test fixture: an upstream tier already failed");
    ASSERT_EQ(rep.errorCount(), 1u);
}

[[nodiscard]] std::string readAll(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{(std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep,
                                    DiagnosticCode            code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

// Every file in `dir` — the artifact AND any staging temp the refusal might
// have left. `writeBytes` claims a sibling `<path>.dsstmp-*`, so "the artifact
// is absent" is not the whole invariant.
[[nodiscard]] std::vector<std::string> namesIn(fs::path const& dir) {
    std::vector<std::string> out;
    std::error_code          ec;
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator{}; it.increment(ec)) {
        out.push_back(it->path().filename().generic_string());
    }
    return out;
}

[[nodiscard]] std::string join(std::vector<std::string> const& v) {
    std::string s;
    for (auto const& e : v) {
        if (!s.empty()) s += ", ";
        s += e;
    }
    return s.empty() ? std::string{"<none>"} : s;
}

}  // namespace

// ── THE CONTROLS ────────────────────────────────────────────────────────────
//
// FIRST, deliberately: every assertion below is about a write NOT happening,
// and a writer that has stopped writing anything at all satisfies them all.

TEST(ArtifactWithheldAfterError, ACleanReporterStillCommitsTheBytes) {
    ScratchDir     scratch{Location::Temp, "withheld-ctl-bytes"};
    auto const     out = scratch.path() / "clean.bin";
    DiagnosticReporter rep;

    ASSERT_TRUE(linker::writeBytes(payload(), out, rep));
    EXPECT_TRUE(fs::exists(out));
    EXPECT_EQ(readAll(out).size(), sizeof(kPayload));
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_ArtifactWithheldAfterError), 0u);
}

TEST(ArtifactWithheldAfterError, ACleanReporterStillCommitsAnImage) {
    ScratchDir     scratch{Location::Temp, "withheld-ctl-image"};
    auto const     out = scratch.path() / "clean.img";
    DiagnosticReporter rep;

    ASSERT_TRUE(linker::writeImage(makeImage(), out, rep));
    EXPECT_TRUE(fs::exists(out));
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_ArtifactWithheldAfterError), 0u);
}

// ── THE INVARIANT ───────────────────────────────────────────────────────────

TEST(ArtifactWithheldAfterError, AReporterHoldingAnErrorWithholdsTheBytes) {
    ScratchDir     scratch{Location::Temp, "withheld-bytes"};
    auto const     out = scratch.path() / "withheld.bin";
    DiagnosticReporter rep;
    recordOneUnrelatedError(rep);

    EXPECT_FALSE(linker::writeBytes(payload(), out, rep))
        << "writeBytes committed an artifact for a compilation that had "
           "already recorded an error";
    EXPECT_FALSE(fs::exists(out));
    EXPECT_EQ(namesIn(scratch.path()).size(), 0u)
        << "the refused write left something behind: "
        << join(namesIn(scratch.path()))
        << " — a staging temp is as much a leftover as the artifact";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_ArtifactWithheldAfterError), 1u)
        << "the withholding must SAY so: a write that silently does nothing "
           "is the same class of defect as one that silently succeeds";
}

TEST(ArtifactWithheldAfterError, AReporterHoldingAnErrorWithholdsTheImage) {
    ScratchDir     scratch{Location::Temp, "withheld-image"};
    auto const     out = scratch.path() / "withheld.img";
    DiagnosticReporter rep;
    recordOneUnrelatedError(rep);

    EXPECT_FALSE(linker::writeImage(makeImage(), out, rep))
        << "writeImage committed an image for a compilation that had already "
           "recorded an error — the delegation to writeBytes is what carries "
           "the invariant, so this case is what proves it is not bypassed";
    EXPECT_FALSE(fs::exists(out));
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_ArtifactWithheldAfterError), 1u);
}

// ── THE SEVERITY CONTRACT ───────────────────────────────────────────────────

TEST(ArtifactWithheldAfterError, TheNoticeDoesNotRaiseTheErrorCount) {
    ScratchDir     scratch{Location::Temp, "withheld-severity"};
    auto const     out = scratch.path() / "withheld.bin";
    DiagnosticReporter rep;
    recordOneUnrelatedError(rep);
    auto const before = rep.errorCount();

    EXPECT_FALSE(linker::writeBytes(payload(), out, rep));
    EXPECT_EQ(rep.errorCount(), before)
        << "the withholding notice counted as a SECOND error. It reports a "
           "CONSEQUENCE of a failure already on the reporter, and every "
           "snapshot gate in the pipeline reads errorCount() deltas — a "
           "notice that moves the count makes one defect look like two and "
           "can trip the diagnostic cap on the way out.";

    bool sawInfo = false;
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::K_ArtifactWithheldAfterError) continue;
        sawInfo = d.severity == DiagnosticSeverity::Info;
    }
    EXPECT_TRUE(sawInfo)
        << "K_ArtifactWithheldAfterError must be Info severity, for the same "
           "reason D_LaterPhasesNotRun is";
}

// ── THE NON-DESTRUCTIVE CLAUSE ──────────────────────────────────────────────

TEST(ArtifactWithheldAfterError, AWithheldWriteLeavesAnExistingFileByteIdentical) {
    ScratchDir scratch{Location::Temp, "withheld-stale"};
    auto const out = scratch.path() / "previous.bin";

    // A previous build's good artifact.
    {
        DiagnosticReporter clean;
        ASSERT_TRUE(linker::writeBytes(payload(), out, clean));
    }
    auto const before = readAll(out);
    ASSERT_FALSE(before.empty());

    DiagnosticReporter rep;
    recordOneUnrelatedError(rep);
    EXPECT_FALSE(linker::writeBytes(std::span<std::uint8_t const>{}, out, rep));

    ASSERT_TRUE(fs::exists(out))
        << "the refused write DELETED a previous good artifact. This compiler "
           "never destroys an artifact it did not write — the references "
           "themselves split on this (all three unlink on a failed LINK, all "
           "three keep it on a failed FRONT END), so there is no unanimous "
           "answer to inherit and the non-destructive half is the one chosen.";
    EXPECT_EQ(readAll(out), before)
        << "the refused write REWROTE a previous good artifact";
    EXPECT_EQ(namesIn(scratch.path()), std::vector<std::string>{"previous.bin"})
        << "the refused write left an extra file beside the previous artifact: "
        << join(namesIn(scratch.path()));
}
