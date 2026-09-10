// ── A CHECKPOINT MUST NOT COST WHAT IT SITS ON ───────────────────────────
//
// `TreeBuilder::checkpoint()` used to COPY the open-frame stack, the scope
// stack and the schema walker's cursor stack. One probe therefore cost
// O(depth), and D nested probes cost Θ(D²) — ✔MEASURED end-to-end through
// the real parser on nested casts, on this tree, at the base commit: 112 MiB
// at 1024, 332 MiB at 2048, 1199 MiB at 4096, 4690 MiB at 8192, with the
// ratio between successive doublings climbing 2.96 → 3.61 → 3.91 toward the
// 4× of a quadratic. That memory wall, not any host-stack limit, is what held
// the shipped speculation ceiling two orders of magnitude below gcc's
// measured working depth.
//
// ⚠⚠ THE INSTRUMENT IS TIME, AND THE TWO INSTRUMENTS IT IS NOT ARE BOTH
// MISTAKES THIS FILE ACTUALLY MADE FIRST.
//
//   ALLOCATION COUNT is the wrong question and answers CLEAN: copying a
//   `vector<Frame>` is ONE allocation whether the vector holds 256 frames or
//   8192. It is the SIZE that moved, not the count.
//
//   ALLOCATED BYTES, via a global `operator new` replaced in this test
//   binary — the `test_tree_visitor.cpp` pattern — is BLIND HERE, and that
//   was ✔MEASURED, not guessed: the first rendition of this file reported a
//   flat **0 bytes** for a checkpoint the base rendition demonstrably copies
//   hundreds of kilobytes into. The engine is a SHARED library
//   (`dsscp-lib`, built SHARED, shipping as `libdsscp.dll`) and Windows has
//   no symbol interposition, so a replacement `operator new` in an
//   executable never sees an allocation made inside a DLL it links. An
//   instrument that reports 0 for everything passes every assertion.
//
//   ⇒ So the measurement is WALL TIME, which crosses the module boundary the
//   way the work does. `test_checkpoint.cpp` already pins a wall-clock bound
//   (`ThousandTokensWithSpeculationUnder50ms`), so the shape has precedent
//   here.
//
// ★ AND THE ASSERTION IS A RATIO ACROSS DEPTHS, NEVER AN ABSOLUTE BOUND. A
// checkpoint legitimately costs a depth-INDEPENDENT amount (the diagnostic
// reporter's rollback token above all). Pinning an absolute number would pin
// that unrelated cost and red on any change to it. What the design claims is
// narrower and testable: THE COST DOES NOT GROW WITH THE DEPTH IT IS TAKEN
// AT. So the same measurement is taken at two depths a factor of 32 apart.
// The copy-based rendition is ~30× slower at the deeper one; the bound here
// is 8×, which is far above scheduler noise and far below the real signal.
//
// ⚠ AND THE INSTRUMENT CHECKS ITSELF. A clock too coarse to resolve the
// shallow arm would make the ratio meaningless while every assertion passed,
// so the shallow arm's own duration is asserted non-zero first. A measured
// zero is a broken instrument, not a fast checkpoint.
//
// ─────────────────────────────────────────────────────────────────────────
// ★★★ AND TIME ALONE IS BLIND ON THE AXIS THIS CHANGE IS ABOUT.
// ─────────────────────────────────────────────────────────────────────────
// The defect was MEASURED in bytes — 332 MiB at 2048 nested casts, 1199 at
// 4096, 4690 at 8192, `std::bad_alloc` at 65536 — and a regression that
// restored the O(depth) COPY while keeping it fast (a cheaper allocator, a
// wider memcpy, more cores) would hold every ratio above green. So the memory
// half is gated too, and by the ONE route that works here: a CHILD PROCESS
// whose peak working set the parent reads. It crosses the DLL boundary the way
// the work does, for the same reason wall time does, and it is per-arm — a
// high-water mark taken inside this binary would carry every earlier test's
// allocations into both arms and flatten the very difference being measured.
//
// The child runs `DISABLED_NestWorkloadForMemoryGate`, selected by
// `--gtest_filter` + `--gtest_also_run_disabled_tests`, and takes its base
// depth from a plain argv flag (gtest leaves unrecognised arguments alone).
// It prints its own peak on stdout, which `spawnAndWaitRedirectStdout` puts in
// a file — a file rather than a pipe for the reason that entry point
// documents at length.

#include "core/substrate/process_spawn.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#endif

using namespace dss;

namespace {

constexpr std::string_view kCfg = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "X", "version": "0.1.0" },
  "keywords": [ { "word": "if", "kind": "IfKw" } ],
  "tokens": {
    "+": [{ "kind": "PlusOp" }],
    ";": [{ "kind": "EndStatement" }]
  },
  "shapes": {
    "root":  { "sequence": [ { "repeat": "stmt" } ] },
    "stmt":  { "sequence": [ "Identifier", "EndStatement" ] }
  }
})JSON";

struct Harness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    RuleId                               root{};
    RuleId                               stmt{};
};

[[nodiscard]] Harness make() {
    auto loaded = GrammarSchema::loadFromText(kCfg);
    EXPECT_TRUE(loaded.has_value());
    Harness h;
    h.src    = SourceBuffer::fromString("a;", "<cost>");
    h.schema = loaded.has_value() ? *loaded : nullptr;
    if (h.schema) {
        h.root = h.schema->rules().find("root");
        h.stmt = h.schema->rules().find("stmt");
    }
    return h;
}

// Nanoseconds for ONE checkpoint+rollback cycle taken with `depth` frames
// open, as the MINIMUM over `reps` batches of `iters` cycles.
//
// The minimum, not the mean: a batch can only be slowed by interference
// (scheduler, another lane's build), never sped up, so the minimum is the
// closest estimate of the work itself and is what keeps a ratio honest on a
// loaded machine.
[[nodiscard]] double cycleNanosAtDepth(Harness const& h, int depth,
                                       int iters = 400, int reps = 5) {
    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault(),
                  BuilderConfig{.maxSpeculationDepth = 64}};
    std::vector<TreeBuilder::OpenScope> frames;
    frames.reserve(static_cast<std::size_t>(depth) + 1);
    frames.push_back(b.open(h.root));
    for (int i = 0; i < depth; ++i) frames.push_back(b.open(h.stmt));

    // Warm-up: let the checkpoint stack, the reporter's rollback token and
    // (in the journal design) the undo buffers reach steady-state capacity,
    // so the measurement is the checkpoint rather than the first allocation.
    for (int i = 0; i < 64; ++i) {
        auto cp = b.checkpoint();
        b.rollback(std::move(cp));
    }

    double best = -1.0;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            auto cp = b.checkpoint();
            b.rollback(std::move(cp));
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())
            / iters;
        if (best < 0.0 || ns < best) best = ns;
    }

    while (!frames.empty()) frames.pop_back();
    Tree t = std::move(b).finish();
    (void)t;
    return best;
}

// ── the memory arm ───────────────────────────────────────────────────────

// This process's own peak resident set, in bytes. The HOST split is not the
// project's language/target/format agnosticism rule — it is three spellings of
// one OS-provided high-water counter, and there is no portable one.
// ⓘ The gate below compares two children measured by THIS function, so even a
// host whose unit conversion were wrong would still produce a sound ratio; the
// normalisation is here for the printed figure.
[[nodiscard]] std::uint64_t peakResidentBytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)) == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
#else
    ::rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#  if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);            // bytes
#  else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024u;    // KiB
#  endif
#endif
}

constexpr std::string_view kBaseDepthFlag = "--nest-base-depth=";
// The number of DISTINCT diagnostic codes to leave standing in the reporter
// before the checkpoints are taken. Zero for the gate below; a non-zero value
// is how the aggregate cost of `CheckpointSnapshot::reporterSnap` — O(1) per
// checkpoint but Θ(probes × distinct-codes) in total — gets measured rather
// than argued about.
constexpr std::string_view kDiagCodesFlag = "--nest-diag-codes=";
constexpr int              kMemoryProbes  = 1024;

// The workload the child runs, and the one the memory claim is about: N
// SIMULTANEOUSLY live checkpoints taken at `baseDepth` frames deep. Under the
// copy-based rendition every one of them copies the whole open-frame stack, so
// the total is Θ(probes × baseDepth) BYTES; under the trail it is Θ(probes).
void runNest(int baseDepth, int diagCodes) {
    auto h = make();
    if (h.schema == nullptr) return;
    TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault(),
                  BuilderConfig{.maxSpeculationDepth = kMemoryProbes + 8}};
    std::vector<TreeBuilder::OpenScope>  frames;
    std::vector<TreeBuilder::Checkpoint> cps;
    frames.reserve(static_cast<std::size_t>(baseDepth + kMemoryProbes) + 1);
    cps.reserve(static_cast<std::size_t>(kMemoryProbes));
    frames.push_back(b.open(h.root));
    // Leave `diagCodes` DISTINCT codes standing in the reporter first. Every
    // checkpoint's `reporterSnap` copies the per-code map and the elision
    // ledger, so this is the axis on which the snapshot is not O(1) — bounded
    // by the reporter's configuration rather than by depth, but paid once per
    // live probe.
    for (int c = 0; c < diagCodes; ++c) {
        ParseDiagnostic d{};
        d.code     = static_cast<DiagnosticCode>(
                         static_cast<int>(DiagnosticCode::P_UnexpectedToken) + c);
        d.severity = DiagnosticSeverity::Warning;
        d.span     = SourceSpan::empty(0);
        d.actual   = "diagnostics-live probe";
        b.reportDiagnostic(std::move(d));
    }
    for (int i = 0; i < baseDepth; ++i) frames.push_back(b.open(h.stmt));
    for (int i = 0; i < kMemoryProbes; ++i) {
        cps.push_back(b.checkpoint());
        frames.push_back(b.open(h.stmt));
    }
    for (int i = kMemoryProbes - 1; i >= 0; --i) {
        frames.pop_back();
        b.rollback(std::move(cps[static_cast<std::size_t>(i)]));
    }
    cps.clear();
    frames.clear();
    Tree t = std::move(b).finish();
    (void)t;
}

[[nodiscard]] std::filesystem::path selfExecutable() {
    auto const& argvs = ::testing::internal::GetArgvs();
    if (argvs.empty()) return {};
    std::error_code ec;
    auto const abs = std::filesystem::absolute(std::filesystem::path{argvs[0]}, ec);
    return ec ? std::filesystem::path{argvs[0]} : abs;
}

// The value of `flag=` in this process's argv, or `fallback` when absent.
[[nodiscard]] int argvInt(std::string_view flag, int fallback) {
    for (auto const& a : ::testing::internal::GetArgvs()) {
        if (a.rfind(flag, 0) == 0) {
            return std::atoi(a.c_str() + flag.size());
        }
    }
    return fallback;
}

// Run the workload in a fresh process at `baseDepth` and return its peak
// resident bytes, or 0 when the measurement could not be taken (the caller
// treats that as a failure, never as a pass).
[[nodiscard]] std::uint64_t childPeakBytes(int baseDepth, std::string& why) {
    namespace fs = std::filesystem;
    const fs::path self = selfExecutable();
    if (self.empty()) { why = "argv[0] is not available"; return 0; }

    std::error_code ec;
    const fs::path out = fs::temp_directory_path(ec)
                       / ("dss-checkpoint-mem-" + std::to_string(baseDepth) + ".txt");
    if (ec) { why = "no temp directory: " + ec.message(); return 0; }

    const std::vector<std::string> argv{
        self.string(),
        "--gtest_filter=CheckpointCost.DISABLED_NestWorkloadForMemoryGate",
        "--gtest_also_run_disabled_tests",
        std::string{kBaseDepthFlag} + std::to_string(baseDepth),
    };
    const auto rc = dss::substrate::spawnAndWaitRedirectStdout(argv, {}, out);
    if (!rc.spawned) { why = "spawn failed: " + rc.diagnostic; return 0; }
    if (rc.exitCode != 0) {
        why = "child exited " + std::to_string(rc.exitCode);
        return 0;
    }
    std::ifstream in{out};
    std::string   token;
    std::uint64_t bytes = 0;
    while (in >> token) {
        if (token == "PEAK_BYTES") { in >> bytes; break; }
    }
    fs::remove(out, ec);
    if (bytes == 0) why = "the child reported no peak";
    return bytes;
}

} // namespace

// The child half of the memory gate. DISABLED so the ordinary suite never runs
// it; the parent selects it explicitly. It measures nothing itself — it exists
// to be a process whose peak the parent can read.
TEST(CheckpointCost, DISABLED_NestWorkloadForMemoryGate) {
    const int baseDepth = argvInt(kBaseDepthFlag, -1);
    ASSERT_GE(baseDepth, 0) << "child started without " << kBaseDepthFlag;
    runNest(baseDepth, argvInt(kDiagCodesFlag, 0));
    std::printf("PEAK_BYTES %llu\n",
                static_cast<unsigned long long>(peakResidentBytes()));
    std::fflush(stdout);
}

TEST(CheckpointCost, ACheckpointCostsTheSameAtDepth256AndDepth8192) {
    auto h = make();
    ASSERT_NE(h.schema, nullptr);

    const double shallow = cycleNanosAtDepth(h, 256);
    const double deep    = cycleNanosAtDepth(h, 8192);

    std::fprintf(stderr,
                 "[cost] checkpoint+rollback: depth 256 -> %.0f ns, "
                 "depth 8192 -> %.0f ns (ratio %.2f)\n",
                 shallow, deep, shallow > 0.0 ? deep / shallow : -1.0);

    ASSERT_GT(shallow, 0.0)
        << "the clock could not resolve the shallow arm — the ratio below "
           "would be meaningless and every assertion would pass anyway";

    // 32× the depth. The copy-based rendition is ~30× slower here; 8× is far
    // above scheduler noise and far below that signal. The 2 µs absolute term
    // keeps a very fast machine from turning a sub-microsecond shallow arm
    // into a hair-trigger.
    EXPECT_LE(deep, shallow * 8.0 + 2000.0)
        << "a checkpoint is still paying for the depth it sits at: "
        << deep << " ns at depth 8192 vs " << shallow << " ns at depth 256";
}

TEST(CheckpointCost, ManySIMULTANEOUSLYLiveProbesStayLinear) {
    // The shape the parser actually runs on a nested-cast chain: probe i sits
    // at depth ~i and every one of them is LIVE at once. Under copy-based
    // checkpoints the total is Θ(D²) — the 4.7 GiB measured end-to-end at
    // 8192. Timed here rather than sized, for the reason in the file header.
    //
    // The two arms differ only in the depth the nest STARTS from, so the
    // number of live checkpoints, the number of frames opened inside them and
    // every other cost are identical; only the depth each checkpoint sits at
    // moves.
    auto h = make();
    ASSERT_NE(h.schema, nullptr);
    constexpr int kProbes = 1024;

    auto nestNanos = [&](int baseDepth) {
        TreeBuilder b{h.src, h.schema, DiagnosticBudget::libraryDefault(),
                      BuilderConfig{.maxSpeculationDepth = kProbes + 8}};
        std::vector<TreeBuilder::OpenScope>  frames;
        std::vector<TreeBuilder::Checkpoint> cps;
        frames.reserve(static_cast<std::size_t>(baseDepth + kProbes) + 1);
        cps.reserve(static_cast<std::size_t>(kProbes));
        frames.push_back(b.open(h.root));
        for (int i = 0; i < baseDepth; ++i) frames.push_back(b.open(h.stmt));

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kProbes; ++i) {
            cps.push_back(b.checkpoint());
            frames.push_back(b.open(h.stmt));
        }
        const auto t1 = std::chrono::steady_clock::now();

        for (int i = kProbes - 1; i >= 0; --i) {
            frames.pop_back();
            b.rollback(std::move(cps[static_cast<std::size_t>(i)]));
        }
        cps.clear();
        frames.clear();
        Tree t = std::move(b).finish();
        (void)t;
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    };

    double shallow = -1.0, deep = -1.0;
    for (int r = 0; r < 3; ++r) {
        const double s = nestNanos(0);
        const double d = nestNanos(8192);
        if (shallow < 0.0 || s < shallow) shallow = s;
        if (deep    < 0.0 || d < deep)    deep    = d;
    }

    std::fprintf(stderr,
                 "[cost] %d simultaneously-live probes: from depth 0 -> %.0f ns, "
                 "from depth 8192 -> %.0f ns (ratio %.2f)\n",
                 kProbes, shallow, deep, shallow > 0.0 ? deep / shallow : -1.0);

    ASSERT_GT(shallow, 0.0) << "the clock could not resolve the shallow arm";
    EXPECT_LE(deep, shallow * 8.0 + 1'000'000.0)
        << kProbes << " live probes cost " << deep << " ns from depth 8192 vs "
        << shallow << " ns from depth 0 — the per-probe cost is still "
           "following the depth";
}

TEST(CheckpointCost, ManyLiveProbesCostTheSameMEMORYAtAnyDepth) {
    // The MEMORY half of the complexity claim, and the reason it is here: the
    // two timing cases above would stay green through a regression that
    // restored the O(depth) copy and merely made it fast. The 332 → 4690 MiB
    // curve is what this change is about, so it gets its own gate.
    //
    // Two children, identical in EVERY respect except the depth their 1024
    // simultaneously-live checkpoints sit at. Same probe count, same frames
    // opened inside them, same rollbacks — only the depth each checkpoint
    // copies (or, now, does not copy) moves.
    std::string whyShallow;
    std::string whyDeep;
    const std::uint64_t shallow = childPeakBytes(0, whyShallow);
    const std::uint64_t deep    = childPeakBytes(8192, whyDeep);

    std::fprintf(stderr,
                 "[cost] %d live probes, child peak RSS: from depth 0 -> "
                 "%.1f MiB, from depth 8192 -> %.1f MiB (ratio %.2f)\n",
                 kMemoryProbes,
                 static_cast<double>(shallow) / (1024.0 * 1024.0),
                 static_cast<double>(deep) / (1024.0 * 1024.0),
                 shallow > 0 ? static_cast<double>(deep)
                             / static_cast<double>(shallow) : -1.0);

    // The instrument checks itself first, exactly as the timing cases do: a
    // host that reported nothing must RED here rather than sail through a
    // comparison of two zeroes.
    ASSERT_GT(shallow, 0u) << "shallow arm unmeasured: " << whyShallow;
    ASSERT_GT(deep,    0u) << "deep arm unmeasured: "    << whyDeep;

    // 1024 checkpoints × 8192 open frames is ~200 MiB of frame copies alone
    // under the copy-based rendition, on top of a process image both arms
    // share. The bound is 1.5× plus 48 MiB of absolute slack — far above the
    // arms' shared cost and the noise in it, far below that signal.
    EXPECT_LE(static_cast<double>(deep),
              static_cast<double>(shallow) * 1.5 + 48.0 * 1024.0 * 1024.0)
        << kMemoryProbes << " live probes peaked at " << deep
        << " B from depth 8192 vs " << shallow
        << " B from depth 0 — a checkpoint is still COPYING the depth it "
           "sits at, whatever the wall clock says";
}
