// ── [[D-FFI-SHIPPED-DESCRIPTORS-DECLARE-STRUCTS-THEIR-OWN-SIGNATURES-DO-NOT-USE]]
//
// THE DEFECT, corpus-wide and not a quirk of one header. ✔MEASURED before the
// fix: across ALL 49 shipped descriptors, NOT ONE symbol signature referenced
// any struct its own descriptor declared. 13 descriptors declare a `structs`
// block; `sys/stat.json` declared `struct stat` with FOUR per-target variants
// and then spelled `fstat`/`lstat`/`stat` plus its eight pe `_stat*` rows over
// `ptr<void>`; `time.json` declared `struct tm` and `struct timespec` per format
// and spelled `localtime`/`gmtime`/`mktime`/`strftime`/`localtime_r`/`gmtime_r`/
// `nanosleep` over `ptr<void>`; `pwd.json`, `utime.json`, `sys/resource.json`,
// `sys/time.json` and `io.json` each did the same for the one or two structs
// they declare. So the mechanism whose entire purpose is to TYPE a shipped
// library's surface was typing none of it.
//
// ★★ THE DEFECT IS THE INTERSECTION, NOT THE `ptr<void>` COUNT, and this file
// pins BOTH SIDES of that distinction on purpose. ✔MEASURED: 641 raw
// `ptr<void>` occurrences exist across the corpus, but only 378 are inside a
// `signature` at all (35 are `$comment` prose and 228 are struct FIELD types —
// `malloc/malloc.json` alone holds 178 of those and declares ZERO symbols). Of
// the 378, the great majority are the REFERENCE type: `malloc` returns one,
// `memcpy` takes them, `read`/`write` take them, Win32's `HANDLE` *is*
// `void *`, and glibc spells `gettimeofday`'s second parameter `void *__tz`.
// A fix that swept `ptr<void>` would have replaced a laxness defect with a
// strictness one, so this file carries an ACCEPTING arm for every genuine
// `void *` it left alone, beside the refusal for every one it typed.
//
// ★★ WHY IT NEEDED A SECOND READER CHANGE, ON TOP OF THE ONE THAT MADE A STRUCT
// SPELLABLE AT ALL. Moving the `structs` decode ahead of `symbols`
// ([[D-FFI-DIRENT-API-DECLARED-OVER-VOID-NOT-ITS-OWN-STRUCTS]]) was necessary
// and not sufficient. `struct stat`'s macho variant names `timespec` BY NAME,
// and the by-name dependency gate treats an incompletely-published name as
// unavailable — so on a read that selects no variant (target-less: LSP, the
// direct API, the `AllShippedDescriptorsDecode` sweep) the `stat` entry was
// SKIPPED ENTIRELY and its tag was never published. ✔MEASURED: the instant
// `fstat` stopped being `ptr<void>`, that read answered `unknown type 'stat'`
// and took the WHOLE `sys/stat.json` read down — three test binaries red at
// once. The remedy is symmetric with the one already there: an entry that
// cannot state a LAYOUT still publishes its TAG, incomplete, whichever of the
// two reasons applies. `AByNameDependencyGateStillPublishesTheTag` below is
// that arm's red-on-disable.
//
// ★ THE LAYOUTS THIS FILE PINS, ✔MEASURED BY EXECUTION 2026-09-03 on three legs
//   (offsetof/sizeof probes, each toolchain run separately):
//     elf x86_64  (gcc 13.3.0, native)           struct stat 144/8, st_mode u32@24
//     elf arm64   (cross gcc, under qemu-aarch64) struct stat 128/8, st_mode u32@16
//     pe x86_64   (mingw-w64 gcc 13.2.0, native)  struct stat  48/8, st_mode u16@6
//                                                 struct _stat64 56, _stat64i32 48
//                                                 struct tm 36/4, timespec 16/8
//                                                 _wfinddata64i32_t 560/8
//     elf both    struct tm 56/8 · timespec 16/8 · timeval 16/8 · rusage 144/8
//                 · utimbuf 16/8 · passwd 48/8
//     macho arm64 (Apple clang 21.0.0, macOS 26.6.2, the operator's own
//                 hardware, reached over the ssh-macos carriage)
//                                                 struct stat 144/8, st_mode u16@4,
//                                                 st_ino u64@8, st_size i64@96,
//                                                 st_blocks@104, st_blksize i32@112
//                                                 · struct tm 56/8 · timespec 16/8
//                                                 · timeval 16/8 with tv_usec 4
//                                                 · rusage 144/8 · utimbuf 16/8
//                                                 · passwd 72/8, pw_dir@48
//   EVERY number asserted below was reproduced by RUNNING a probe on the leg it
//   describes — all FOUR legs, including macho. The hedge that stood here ("the
//   macho figures are the DESCRIPTORS' OWN, re-asserted rather than re-measured:
//   no macOS host was reached by this lane") was true when written and is now
//   discharged; it is replaced by the measurement rather than deleted, because a
//   reader needs to know which numbers have a probe behind them.
//
// ⚠ WHAT THE CORPUS EXAMPLE CANNOT REACH, WHICH IS WHY THIS FILE EXISTS. A
// RETURN type shows up in an expression's type, so
// `examples/c/shipped_stat_typed_surface` interrogates it by execution. A
// PARAMETER type has NO runtime signature at all — `struct stat *` converts to
// `void *` and back silently (C 6.3.2.3p1) — so no runnable program can tell
// the two spellings apart, and the parameter half of this row is observable
// ONLY as a refusal. Refusals live here, each beside its nearest ACCEPTING
// twin, because a type check fails in two opposite directions and only the pair
// separates a fix that landed from one that over-reached.

#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using namespace dss::sem_test;

namespace fs = std::filesystem;

namespace {

constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

[[nodiscard]] fs::path shippedDescriptor(std::string_view relative) {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    fs::path p = *cfg / "shippedLibs";
    for (auto const& part : fs::path{relative}) p /= part;
    return p;
}

// One descriptor read, exactly as the semantic phase performs it for a target.
// The DATA MODEL travels with the format (LLP64 on pe, LP64 elsewhere) because
// some typedef variants are keyed on it, and a read that got it wrong would
// select a different body and answer a different question.
struct Read {
    TypeInterner                        interner{CompilationUnitId{1}};
    TypeRegistry                        typeReg;
    DiagnosticReporter                  rep;
    std::optional<ShippedLibDescriptor> desc;
    DataModel                           dm{DataModel::Lp64};

    Read(std::string_view relative, std::string_view arch,
         std::optional<ObjectFormatKind> fmt) {
        dm = (fmt.has_value() && *fmt == ObjectFormatKind::Pe) ? DataModel::Llp64
                                                               : DataModel::Lp64;
        fs::path const path = shippedDescriptor(relative);
        if (path.empty()) return;
        desc = readShippedLibDescriptor(
            path, interner, typeReg, rep, dm,
            arch.empty() ? std::optional<std::string_view>{}
                         : std::optional<std::string_view>{arch},
            fmt);
    }

    [[nodiscard]] ShippedSymbol const* symbol(std::string_view name) const {
        if (!desc.has_value()) return nullptr;
        for (auto const& s : desc->symbols)
            if (s.name == name) return &s;
        return nullptr;
    }

    // Every row carrying this name — `sys/stat.json` declares `stat` and `fstat`
    // TWICE each (an elf/macho row and a pe row), so a lookup that stopped at
    // the first would silently measure the wrong one on one of the two formats.
    [[nodiscard]] std::vector<ShippedSymbol const*> allRows(
        std::string_view name) const {
        std::vector<ShippedSymbol const*> out;
        if (!desc.has_value()) return out;
        for (auto const& s : desc->symbols)
            if (s.name == name) out.push_back(&s);
        return out;
    }

    [[nodiscard]] TypeId returnPointee(std::string_view name) const {
        auto const* s = symbol(name);
        if (s == nullptr || !s->signature.valid()) return TypeId{};
        TypeId const ret = interner.fnResult(s->signature);
        if (!ret.valid() || interner.kind(ret) != TypeKind::Ptr) return TypeId{};
        auto const ops = interner.operands(ret);
        return ops.empty() ? TypeId{} : ops[0];
    }

    [[nodiscard]] TypeId paramPointeeOf(ShippedSymbol const* s,
                                        std::size_t i) const {
        if (s == nullptr || !s->signature.valid()) return TypeId{};
        auto const params = interner.fnParams(s->signature);
        if (i >= params.size()) return TypeId{};
        TypeId const p = params[i];
        if (!p.valid() || interner.kind(p) != TypeKind::Ptr) return TypeId{};
        auto const ops = interner.operands(p);
        return ops.empty() ? TypeId{} : ops[0];
    }

    [[nodiscard]] TypeId paramPointee(std::string_view name,
                                      std::size_t i) const {
        return paramPointeeOf(symbol(name), i);
    }

    [[nodiscard]] TypeId structNamed(std::string_view name) const {
        if (!desc.has_value()) return TypeId{};
        for (auto const& s : desc->structs)
            if (s.name == name) return s.typeId;
        return TypeId{};
    }
};

// The one assertion this whole row is about, factored so every descriptor gets
// the SAME question asked of it: this pointer's pointee must be the very struct
// the SAME descriptor injects — not void, and not a look-alike interned twice.
void expectPointsAtOwnStruct(Read const& r, TypeId pointee,
                             std::string_view structName, char const* what) {
    TypeId const injected = r.structNamed(structName);
    ASSERT_TRUE(injected.valid()) << what << ": no struct '" << structName
                                  << "' injected by its own descriptor";
    ASSERT_TRUE(pointee.valid()) << what << ": not a pointer at all";
    EXPECT_NE(r.interner.kind(pointee), TypeKind::Void)
        << what << ": THE ROW — this must not be typed over void";
    EXPECT_EQ(pointee, injected)
        << what << ": must point at the SAME struct this descriptor injects, "
                   "not a second one that could silently drift from it";
}

void expectLayout(Read const& r, TypeId ty, std::uint64_t size,
                  std::size_t fieldIndex, std::uint64_t fieldOffset,
                  char const* what) {
    auto const layout = computeLayout(ty, r.interner, kNatural16, r.dm);
    ASSERT_TRUE(layout.has_value()) << what;
    EXPECT_EQ(layout->size, size) << what << ": sizeof";
    ASSERT_GT(layout->fieldOffsets.size(), fieldIndex) << what;
    EXPECT_EQ(layout->fieldOffsets[fieldIndex], fieldOffset)
        << what << ": field offset — a size check alone cannot discriminate a "
                   "wrong layout of the right size";
}

}  // namespace

// ══ 1. sys/stat.json — THE CERTAIN CASE, AND THE ONE THE ROW NAMED FIRST ═════

// `fstat`/`lstat`/`stat` take a pointer to THE VERY `struct stat` this
// descriptor injects, on every served target, with that target's real layout.
// The four legs disagree on size AND field order, which is exactly why the
// by-name spelling is the fix and an inline body would have been a fresh defect.
TEST(ShippedStatTypedSurface, StatFamilyTakesTheDescriptorsOwnStatPerTarget) {
    struct Case {
        ObjectFormatKind fmt;
        char const*      arch;
        std::uint64_t    size;
        std::size_t      modeIndex;
        std::uint64_t    modeOffset;
        bool             hasLstat;
    };
    Case const cases[] = {
        {ObjectFormatKind::Elf,   "x86_64", 144u, 3u, 24u, true},
        {ObjectFormatKind::Elf,   "arm64",  128u, 2u, 16u, true},
        {ObjectFormatKind::MachO, "arm64",  144u, 1u,  4u, true},
        {ObjectFormatKind::Pe,    "x86_64",  48u, 2u,  6u, false},
    };
    for (auto const& c : cases) {
        Read r{"sys/stat.json", c.arch, c.fmt};
        ASSERT_TRUE(r.desc.has_value()) << c.arch;
        EXPECT_FALSE(r.rep.hasErrors()) << c.arch;

        // EVERY row of each name, because this descriptor carries two `stat`
        // rows and two `fstat` rows (elf/macho and pe) and only one of each is
        // active per format — but both DECODE on every read, so a mistyped
        // inactive row would lurk.
        for (auto const* row : r.allRows("fstat"))
            expectPointsAtOwnStruct(r, r.paramPointeeOf(row, 1), "stat",
                                    "fstat param 1");
        for (auto const* row : r.allRows("stat"))
            expectPointsAtOwnStruct(r, r.paramPointeeOf(row, 1), "stat",
                                    "stat param 1");
        if (c.hasLstat)
            expectPointsAtOwnStruct(r, r.paramPointee("lstat", 1), "stat",
                                    "lstat param 1");

        expectLayout(r, r.structNamed("stat"), c.size, c.modeIndex, c.modeOffset,
                     c.arch);
    }
}

// ★★ THE UCRT TAG SPLIT, WHICH THE RETYPE HAD TO GET RIGHT AND WHICH A SWEEP
// WOULD HAVE FLATTENED. UCRT's own <sys/stat.h> declares
// `_stat64i32(char const*, struct _stat64i32*)` and
// `_stat64(char const*, struct _stat64*)`, and `#define _stat _stat64i32`, so
// this descriptor's `_stat` entry IS the `_stat64i32` body. Its `stat()` and
// `fstat()` are INLINE FORWARDERS that CAST `struct stat *` to
// `struct _stat64i32 *` — the reference's own admission that the two tags are
// distinct types of identical layout. So the user-facing rows take `stat` and
// the underscore rows take their own tags, and the 48-vs-56 layout split is
// asserted here so "distinct" is not merely a spelling.
TEST(ShippedStatTypedSurface, ThePeUnderscoreRowsTakeTheirOwnUcrtTags) {
    Read r{"sys/stat.json", "x86_64", ObjectFormatKind::Pe};
    ASSERT_TRUE(r.desc.has_value());
    EXPECT_FALSE(r.rep.hasErrors());

    for (char const* n : {"_stat64i32", "_wstat64i32"})
        expectPointsAtOwnStruct(r, r.paramPointee(n, 1), "_stat", n);
    expectPointsAtOwnStruct(r, r.paramPointee("_fstat64i32", 1), "_stat",
                            "_fstat64i32");
    for (char const* n : {"_stat64", "_wstat64"})
        expectPointsAtOwnStruct(r, r.paramPointee(n, 1), "_stat64", n);
    expectPointsAtOwnStruct(r, r.paramPointee("_fstat64", 1), "_stat64",
                            "_fstat64");

    // ✔MEASURED with mingw-w64 gcc 13.2.0: 48 and 56. The two tags are not
    // interchangeable, and this is what makes the split above load-bearing
    // rather than cosmetic.
    expectLayout(r, r.structNamed("_stat"),   48u, 2u, 6u, "pe struct _stat");
    expectLayout(r, r.structNamed("_stat64"), 56u, 2u, 6u, "pe struct _stat64");
    EXPECT_NE(r.structNamed("stat"), r.structNamed("_stat64"))
        << "the 64-bit-size tag must not be the same type as `struct stat`";
    EXPECT_NE(r.structNamed("stat"), r.structNamed("_stat"))
        << "identical FIELDS must not collapse two distinct UCRT tags into one "
           "type — the reference itself casts between them";
}

// ★★★ THE READER FIX'S OWN RED-ON-DISABLE. A read that cannot state a LAYOUT
// still publishes the TAG, whichever of the TWO reasons applies:
//   (a) no `variants` entry matched the active target;
//   (b) a by-name DEPENDENCY of the entry is unavailable on this read.
// `struct stat` hits (b) — its macho variant names `timespec`, which is itself
// only incompletely published on a read that selects no variant — and (b) had
// no publication arm until P56 lane sv. Both arms below therefore assert the
// same three things: the read SUCCEEDS, NO layout is injected (so `out.structs`
// stays byte-identical and nothing downstream can see an incomplete type where
// it saw a complete one), and `ptr<stat>` is nonetheless a well-formed pointer
// to an INCOMPLETE `stat`.
//
// RED-ON-DISABLE: delete the publication in the dependency-unavailable arm of
// the reader's struct loop and both arms report `unknown type 'stat'`, taking
// the whole descriptor read down with them.
TEST(ShippedStatTypedSurface, AByNameDependencyGateStillPublishesTheTag) {
    struct Arm {
        char const*                     what;
        char const*                     arch;
        std::optional<ObjectFormatKind> fmt;
    };
    Arm const arms[] = {
        {"target-less read (LSP / direct API / the decode sweep)", "",
         std::nullopt},
        {"a format <sys/stat.h> does not serve", "x86_64",
         std::optional<ObjectFormatKind>{ObjectFormatKind::Wasm}},
    };
    for (auto const& arm : arms) {
        Read r{"sys/stat.json", arm.arch, arm.fmt};
        ASSERT_TRUE(r.desc.has_value())
            << arm.what << ": a read that cannot state a layout must not fail";
        EXPECT_FALSE(r.rep.hasErrors()) << arm.what;
        EXPECT_TRUE(r.desc->structs.empty())
            << arm.what << ": no variant matched, so no layout may be injected";

        for (auto const* row : r.allRows("fstat")) {
            TypeId const pointee = r.paramPointeeOf(row, 1);
            ASSERT_TRUE(pointee.valid())
                << arm.what << ": `ptr<stat>` must still be a pointer type";
            EXPECT_EQ(r.interner.kind(pointee), TypeKind::Struct) << arm.what;
            EXPECT_EQ(r.interner.name(pointee), "stat") << arm.what;
            EXPECT_TRUE(r.interner.isIncompleteComposite(pointee))
                << arm.what
                << ": the tag exists; only its LAYOUT is unknowable here";
        }
    }
}

// ══ 2. time.json — the largest true intersection, and the UCRT argument order ═

TEST(ShippedStatTypedSurface, TimeRowsAreTypedOverTmAndTimespec) {
    struct Case {
        ObjectFormatKind fmt;
        char const*      arch;
        std::uint64_t    tmSize;
        bool             posix;   // localtime_r / gmtime_r / nanosleep
    };
    Case const cases[] = {
        {ObjectFormatKind::Elf,   "x86_64", 56u, true},
        {ObjectFormatKind::Elf,   "arm64",  56u, true},
        {ObjectFormatKind::MachO, "arm64",  56u, true},
        {ObjectFormatKind::Pe,    "x86_64", 36u, false},
    };
    for (auto const& c : cases) {
        Read r{"time.json", c.arch, c.fmt};
        ASSERT_TRUE(r.desc.has_value()) << c.arch;
        EXPECT_FALSE(r.rep.hasErrors()) << c.arch;

        // The RETURN half — the only half a runnable program can observe.
        for (char const* n : {"localtime", "gmtime"})
            for (auto const* row : r.allRows(n))
                expectPointsAtOwnStruct(
                    r,
                    [&] {
                        TypeId const ret = r.interner.fnResult(row->signature);
                        if (!ret.valid()
                            || r.interner.kind(ret) != TypeKind::Ptr)
                            return TypeId{};
                        auto const ops = r.interner.operands(ret);
                        return ops.empty() ? TypeId{} : ops[0];
                    }(),
                    "tm", n);
        if (r.symbol("_localtime64") != nullptr)
            expectPointsAtOwnStruct(r, r.returnPointee("_localtime64"), "tm",
                                    "_localtime64");

        // The PARAMETER half.
        for (auto const* row : r.allRows("mktime"))
            expectPointsAtOwnStruct(r, r.paramPointeeOf(row, 0), "tm",
                                    "mktime param 0");
        expectPointsAtOwnStruct(r, r.paramPointee("strftime", 3), "tm",
                                "strftime param 3");

        if (c.posix) {
            for (char const* n : {"localtime_r", "gmtime_r"})
                expectPointsAtOwnStruct(r, r.paramPointee(n, 1), "tm",
                                        "the _r out-parameter");
            expectPointsAtOwnStruct(r, r.paramPointee("nanosleep", 0),
                                    "timespec", "nanosleep param 0");
            expectPointsAtOwnStruct(r, r.paramPointee("nanosleep", 1),
                                    "timespec", "nanosleep param 1");
        } else {
            // ⚠ THE UCRT ORDER IS THE OPPOSITE OF POSIX's AND THIS IS THE PIN
            // THAT SAYS SO. `errno_t _localtime64_s(struct tm* _Tm,
            // __time64_t const* _Time)` — the STRUCT IS FIRST, where POSIX's
            // `localtime_r(const time_t*, struct tm*)` has it second. Read
            // directly from the Windows SDK's <time.h>. Typing the wrong
            // parameter would have compiled and been wrong.
            for (char const* n : {"_localtime64_s", "_gmtime64_s",
                                  "localtime_s", "gmtime_s"})
                expectPointsAtOwnStruct(r, r.paramPointee(n, 0), "tm", n);
        }

        expectLayout(r, r.structNamed("tm"), c.tmSize, 8u, 32u, c.arch);
    }
}

// ⚠ THE RESIDUAL THIS ROW DOES NOT CLOSE, PINNED SO IT CANNOT BE MISTAKEN FOR
// AN OVERSIGHT. `time`, `localtime`, `gmtime` and the four `*_s` rows take a
// `time_t *`, and `time_t` is a TYPEDEF this descriptor declares — so those
// twelve positions look like the same defect. THEY ARE NOT REACHABLE THE SAME
// WAY, and the reason is measured: the reader's typedef arm injects NOTHING
// when no `variants` entry matches (`if (!selected) continue;`), and `time_t`'s
// variants are keyed on the DATA MODEL as well as the format. On a target-less
// read none match, so `ptr<time_t>` would answer `unknown type 'time_t'` and
// take the descriptor down. The struct remedy cannot be mirrored: an incomplete
// TAG is still a type, whereas a typedef with no selected type has no type at
// all. This test pins that those rows are STILL `ptr<void>`, so the day a
// mechanism for it lands, this assertion is what says the residual is gone.
TEST(ShippedStatTypedSurface, TheTimeTPositionsRemainVoidAndThatIsMeasuredNotLazy) {
    Read r{"time.json", "x86_64", ObjectFormatKind::Elf};
    ASSERT_TRUE(r.desc.has_value());
    for (char const* n : {"time", "localtime", "gmtime"}) {
        for (auto const* row : r.allRows(n)) {
            TypeId const pointee = r.paramPointeeOf(row, 0);
            ASSERT_TRUE(pointee.valid()) << n;
            EXPECT_EQ(r.interner.kind(pointee), TypeKind::Void)
                << n << ": the time_t* positions are a DIFFERENT defect needing "
                        "a typedef-side mechanism that does not exist yet; when "
                        "it lands, retype them and update this pin";
        }
    }
    // The `time_t` typedef itself IS declared and DOES resolve for a real
    // target — which is what makes the residual a reader limitation rather than
    // a missing declaration.
    bool sawTimeT = false;
    for (auto const& t : r.desc->typedefs)
        if (t.name == "time_t") sawTimeT = true;
    EXPECT_TRUE(sawTimeT) << "time_t must be declared — the gap is the reader's "
                             "no-variant-match behaviour, not the descriptor's";
}

// ══ 3. THE FOUR SMALL DESCRIPTORS, AND THE `void *` LEFT ALONE BESIDE THEM ═══

TEST(ShippedStatTypedSurface, TheRemainingDescriptorsAreTypedOverTheirOwnStructs) {
    {   // pwd.json — `struct passwd *getpwuid(uid_t)`; elf 48 B, macho 72 B.
        Read elf{"pwd.json", "x86_64", ObjectFormatKind::Elf};
        ASSERT_TRUE(elf.desc.has_value());
        expectPointsAtOwnStruct(elf, elf.returnPointee("getpwuid"), "passwd",
                                "getpwuid return");
        expectLayout(elf, elf.structNamed("passwd"), 48u, 2u, 16u, "elf passwd");
        Read mach{"pwd.json", "arm64", ObjectFormatKind::MachO};
        ASSERT_TRUE(mach.desc.has_value());
        expectPointsAtOwnStruct(mach, mach.returnPointee("getpwuid"), "passwd",
                                "getpwuid return, macho");
        expectLayout(mach, mach.structNamed("passwd"), 72u, 7u, 48u,
                     "macho passwd — pw_dir moves to 48");
    }
    {   // utime.json — `int utime(const char*, const struct utimbuf*)`; FLAT,
        // so there is no variant to select and the tag is complete on any read.
        Read r{"utime.json", "x86_64", ObjectFormatKind::Elf};
        ASSERT_TRUE(r.desc.has_value());
        expectPointsAtOwnStruct(r, r.paramPointee("utime", 1), "utimbuf",
                                "utime param 1");
        expectLayout(r, r.structNamed("utimbuf"), 16u, 1u, 8u, "utimbuf");
    }
    {   // sys/resource.json — `int getrusage(int, struct rusage *)`.
        Read r{"sys/resource.json", "x86_64", ObjectFormatKind::Elf};
        ASSERT_TRUE(r.desc.has_value());
        expectPointsAtOwnStruct(r, r.paramPointee("getrusage", 1), "rusage",
                                "getrusage param 1");
        expectLayout(r, r.structNamed("rusage"), 144u, 1u, 16u,
                     "rusage — ru_stime at 16");
    }
    {   // sys/time.json — the DISCRIMINATING pair. `gettimeofday`'s FIRST
        // parameter is `struct timeval *`; its SECOND is spelled `void *__tz`
        // by glibc itself (✔read in WSL, x86_64 AND the aarch64 cross headers)
        // and `void * __restrict` by the macOS SDK. So one parameter of ONE
        // signature is typed and the other is deliberately not — which is the
        // whole census in miniature, and the reason a sweep would have been
        // wrong.
        Read r{"sys/time.json", "x86_64", ObjectFormatKind::Elf};
        ASSERT_TRUE(r.desc.has_value());
        expectPointsAtOwnStruct(r, r.paramPointee("gettimeofday", 0), "timeval",
                                "gettimeofday param 0");
        TypeId const tz = r.paramPointee("gettimeofday", 1);
        ASSERT_TRUE(tz.valid());
        EXPECT_EQ(r.interner.kind(tz), TypeKind::Void)
            << "gettimeofday's `tz` is `void *` IN THE REFERENCE — typing it "
               "would be an invented extension, not a fix";
        expectPointsAtOwnStruct(r, r.paramPointee("utimes", 1), "timeval",
                                "utimes param 1");
        Read mach{"sys/time.json", "arm64", ObjectFormatKind::MachO};
        ASSERT_TRUE(mach.desc.has_value());
        expectPointsAtOwnStruct(mach, mach.paramPointee("futimes", 1), "timeval",
                                "futimes param 1 (macho-only row)");
    }
    {   // io.json — pe only. The find pair is typed; the read/write trio is
        // NOT, because `_read`/`read`/`write` genuinely take `void *`.
        Read r{"io.json", "x86_64", ObjectFormatKind::Pe};
        ASSERT_TRUE(r.desc.has_value());
        expectPointsAtOwnStruct(r, r.paramPointee("_wfindfirst64i32", 1),
                                "_wfinddata_t", "_wfindfirst64i32 param 1");
        expectPointsAtOwnStruct(r, r.paramPointee("_wfindnext64i32", 1),
                                "_wfinddata_t", "_wfindnext64i32 param 1");
        expectLayout(r, r.structNamed("_wfinddata_t"), 560u, 5u, 36u,
                     "pe _wfinddata64i32_t — name[260] at 36");
        for (char const* n : {"_read", "read", "write"}) {
            TypeId const buf = r.paramPointee(n, 1);
            ASSERT_TRUE(buf.valid()) << n;
            EXPECT_EQ(r.interner.kind(buf), TypeKind::Void)
                << n << ": a real `void *` buffer parameter must be LEFT ALONE";
        }
    }
}

// ══ 3b. windows.json — the largest block, and the one where `void *` REALLY IS
//        the reference type for most of the surface ════════════════════════════

// ★★ THIS DESCRIPTOR IS WHERE THE CENSUS EARNS ITS KEEP. It carries 79
// void-signatures and 121 occurrences — a third of the corpus's signature-level
// total — and the great majority are CORRECT: `typedef void *HANDLE`, and
// `LPVOID`/`LPCVOID`/`PVOID` are `void *` by definition. Only the rows naming a
// struct this file declares were the defect. So this test asserts BOTH lists,
// and the second list is what makes the first falsifiable.
//
// ★ IT ALSO FIXES THE DEFECT ONE LEVEL DOWN, WHICH THE SIGNATURES ALONE WOULD
// HAVE LEFT: the POINTER TYPEDEFS were themselves `ptr<void>`
// (`PSRWLOCK`, `LPCRITICAL_SECTION`, `PCONDITION_VARIABLE`, `LPSYSTEM_INFO`,
// `PLARGE_INTEGER`, `LPOVERLAPPED`, `LPSYSTEMTIME`, `LPWIN32_FIND_DATAW`), so a
// TU writing `PSRWLOCK p;` got a `void *` even though the same file declared
// `SRWLOCK` correctly. They are by-name now, which works here — and NOT for
// `time_t` — because each pointee typedef is declared EARLIER in the same
// `typedefs` array, and the reader publishes each name as it resolves.
// `LPFILETIME` additionally SHED a duplicated inline body (it restated
// `struct "FILETIME" {…}` in the typedef, a second copy able to drift from the
// `structs` entry).
TEST(ShippedStatTypedSurface, WindowsRowsAreTypedOverTheStructsThisFileDeclares) {
    Read r{"windows.json", "x86_64", ObjectFormatKind::Pe};
    ASSERT_TRUE(r.desc.has_value());
    EXPECT_FALSE(r.rep.hasErrors());

    struct Slot { char const* fn; std::size_t param; char const* tag; };
    Slot const typed[] = {
        {"InitializeSRWLock", 0, "SRWLOCK"},
        {"AcquireSRWLockExclusive", 0, "SRWLOCK"},
        {"ReleaseSRWLockExclusive", 0, "SRWLOCK"},
        {"TryAcquireSRWLockExclusive", 0, "SRWLOCK"},
        {"InitializeCriticalSection", 0, "CRITICAL_SECTION"},
        {"DeleteCriticalSection", 0, "CRITICAL_SECTION"},
        {"EnterCriticalSection", 0, "CRITICAL_SECTION"},
        {"LeaveCriticalSection", 0, "CRITICAL_SECTION"},
        {"TryEnterCriticalSection", 0, "CRITICAL_SECTION"},
        {"InitializeConditionVariable", 0, "CONDITION_VARIABLE"},
        {"WakeConditionVariable", 0, "CONDITION_VARIABLE"},
        {"WakeAllConditionVariable", 0, "CONDITION_VARIABLE"},
        {"SleepConditionVariableCS", 0, "CONDITION_VARIABLE"},
        {"SleepConditionVariableCS", 1, "CRITICAL_SECTION"},
        {"GetSystemInfo", 0, "SYSTEM_INFO"},
        {"GetSystemTimeAsFileTime", 0, "FILETIME"},
        {"GetSystemTimePreciseAsFileTime", 0, "FILETIME"},
        {"QueryPerformanceCounter", 0, "LARGE_INTEGER"},
        {"GetFileSizeEx", 1, "LARGE_INTEGER"},
        {"SetFilePointerEx", 2, "LARGE_INTEGER"},
        {"ReadFile", 4, "OVERLAPPED"},
        {"WriteFile", 4, "OVERLAPPED"},
        {"LockFileEx", 5, "OVERLAPPED"},
        {"UnlockFileEx", 4, "OVERLAPPED"},
        {"FindFirstFileW", 1, "WIN32_FIND_DATAW"},
        {"FindNextFileW", 1, "WIN32_FIND_DATAW"},
        {"GetConsoleScreenBufferInfo", 1, "CONSOLE_SCREEN_BUFFER_INFO"},
        {"GetSystemTime", 0, "SYSTEMTIME"},
        {"SystemTimeToFileTime", 0, "SYSTEMTIME"},
        {"SystemTimeToFileTime", 1, "FILETIME"},
        {"SetFileTime", 1, "FILETIME"},
        {"SetFileTime", 2, "FILETIME"},
        {"SetFileTime", 3, "FILETIME"},
    };
    for (auto const& s : typed) {
        TypeId const p = r.paramPointee(s.fn, s.param);
        ASSERT_TRUE(p.valid()) << s.fn << " param " << s.param;
        EXPECT_NE(r.interner.kind(p), TypeKind::Void)
            << s.fn << " param " << s.param << ": THE ROW";
        EXPECT_EQ(r.interner.kind(p), TypeKind::Struct) << s.fn;
        EXPECT_EQ(r.interner.name(p), s.tag)
            << s.fn << " param " << s.param << " must name " << s.tag;
        // …and it must be the SAME type the `structs` surface injects, not a
        // second interning of the same body. This descriptor declares each of
        // these tags TWICE — once as a typedef with an inline body, once as a
        // `structs` entry — so this assertion is what proves the two agree
        // rather than merely coinciding in spelling.
        EXPECT_EQ(p, r.structNamed(s.tag))
            << s.fn << " param " << s.param
            << ": the signature's tag and the `structs` entry must be ONE type";
    }

    // ⚠⚠ THE OTHER HALF OF THE CENSUS, AND WITHOUT IT THE LIST ABOVE PROVES
    // NOTHING. `HANDLE` IS `void *` in Win32, and so are `LPVOID`/`LPCVOID`.
    // These positions must STAY void — a sweep over `ptr<void>` would have
    // "fixed" them into an invented extension.
    struct Genuine { char const* fn; std::size_t param; char const* why; };
    Genuine const leaveAlone[] = {
        {"CloseHandle", 0, "HANDLE is void *"},
        {"ReadFile", 0, "HANDLE"},
        {"ReadFile", 1, "LPVOID buffer"},
        {"WriteFile", 0, "HANDLE"},
        {"WriteFile", 1, "LPCVOID buffer"},
        {"SetFileTime", 0, "HANDLE"},
        {"GetFileSizeEx", 0, "HANDLE"},
        {"HeapFree", 0, "HANDLE (heap)"},
        {"HeapFree", 2, "LPVOID block"},
        {"GetFileAttributesExW", 2, "LPVOID out-buffer — genuinely untyped"},
        {"FlsSetValue", 1, "PVOID slot value"},
        {"UnmapViewOfFile", 0, "LPCVOID base address"},
    };
    for (auto const& g : leaveAlone) {
        TypeId const p = r.paramPointee(g.fn, g.param);
        ASSERT_TRUE(p.valid()) << g.fn << " param " << g.param;
        EXPECT_EQ(r.interner.kind(p), TypeKind::Void)
            << g.fn << " param " << g.param << ": " << g.why
            << " — this `ptr<void>` is the REFERENCE type and must be LEFT ALONE";
    }

    // The pointer TYPEDEFS, which a signature-only fix would have left as
    // `void *` for every TU that spells them.
    struct Alias { char const* alias; char const* tag; };
    Alias const aliases[] = {
        {"PSRWLOCK", "SRWLOCK"},
        {"PCRITICAL_SECTION", "CRITICAL_SECTION"},
        {"LPCRITICAL_SECTION", "CRITICAL_SECTION"},
        {"PCONDITION_VARIABLE", "CONDITION_VARIABLE"},
        {"LPSYSTEM_INFO", "SYSTEM_INFO"},
        {"PLARGE_INTEGER", "LARGE_INTEGER"},
        {"LPOVERLAPPED", "OVERLAPPED"},
        {"LPSYSTEMTIME", "SYSTEMTIME"},
        {"LPWIN32_FIND_DATAW", "WIN32_FIND_DATAW"},
        {"LPFILETIME", "FILETIME"},
    };
    for (auto const& a : aliases) {
        TypeId aliasTy{};
        for (auto const& t : r.desc->typedefs)
            if (t.name == a.alias) aliasTy = t.type;
        ASSERT_TRUE(aliasTy.valid()) << a.alias << " must be declared";
        ASSERT_EQ(r.interner.kind(aliasTy), TypeKind::Ptr) << a.alias;
        auto const ops = r.interner.operands(aliasTy);
        ASSERT_EQ(ops.size(), 1u) << a.alias;
        EXPECT_NE(r.interner.kind(ops[0]), TypeKind::Void)
            << a.alias << ": a pointer alias to a struct this file DECLARES "
                          "must not be `void *`";
        EXPECT_EQ(r.interner.name(ops[0]), a.tag) << a.alias;
    }

    // …and the aliases that must STAY `void *`, for the same reason as above.
    for (char const* v : {"HANDLE", "PVOID", "LPVOID", "LPCVOID",
                          "LPSECURITY_ATTRIBUTES"}) {
        TypeId ty{};
        for (auto const& t : r.desc->typedefs)
            if (t.name == v) ty = t.type;
        ASSERT_TRUE(ty.valid()) << v;
        ASSERT_EQ(r.interner.kind(ty), TypeKind::Ptr) << v;
        auto const ops = r.interner.operands(ty);
        ASSERT_EQ(ops.size(), 1u) << v;
        EXPECT_EQ(r.interner.kind(ops[0]), TypeKind::Void)
            << v << ": `void *` IS this Win32 type (no SECURITY_ATTRIBUTES "
                    "struct is declared here, so its alias states nothing it "
                    "cannot back)";
    }
}

// ══ 4. THE SEMANTIC TIER — the refusals, each beside its accepting twin ══════

namespace {

[[nodiscard]] SemanticModel analyzeShipped(std::string src, ObjectFormatKind fmt,
                                           DataModel dm, char const* arch) {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) throw std::runtime_error(dss::test::configRootDiagnostic());
    auto schema = loadShippedSchema("c");
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(*cfg / "shippedLibs");
    builder.setActiveFormat(fmt);
    builder.addInMemory(std::move(src), "main.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), dm, std::nullopt,
                   std::nullopt, fmt, arch);
}

[[nodiscard]] SemanticModel elfC(std::string src) {
    return analyzeShipped(std::move(src), ObjectFormatKind::Elf, DataModel::Lp64,
                          "x86_64");
}

[[nodiscard]] SemanticModel peC(std::string src) {
    return analyzeShipped(std::move(src), ObjectFormatKind::Pe,
                          DataModel::Llp64, "x86_64");
}

}  // namespace

// ★★★ THE CLOSING TEST THE ROW NAMES, PARAMETER SIDE. A pointer of the wrong
// type must no longer reach `stat`/`fstat`/`lstat`. The twin — the shape every
// real caller is written in — must stay accepted, or the claim is refusing
// everything rather than judging anything.
//
// ★ THE ASSERTION IS ON THE DIAGNOSTIC, NEVER ON ITS SEVERITY, AND THAT IS A
//   MEASURED DECISION RATHER THAN A HEDGE. gcc, clang and mingw-w64 each
//   DIAGNOSE an incompatible object-pointer argument and each EXIT 0; DSS
//   answers an error. That strictness is the pre-existing house posture for
//   every such assignment — the FILE control below proves it on a surface typed
//   long before this row — so pinning Error severity here would pin a
//   divergence this lane did not introduce and would red the day it is
//   correctly relaxed. What the row claims, and what all references agree on,
//   is that the program must not pass in SILENCE.
TEST(ShippedStatTypedSurface, AWrongPointerNoLongerReachesTheStatFamily) {
    for (char const* call : {"stat(\".\", bad)", "lstat(\".\", bad)",
                             "fstat(0, bad)"}) {
        std::string const src = std::string{"#include <sys/stat.h>\n"}
                                + "int main(void){ int *bad = 0; return "
                                + call + "; }\n";
        auto const m = elfC(src);
        EXPECT_TRUE(hasCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch))
            << call << ": a void-typed parameter could not see this at all";
    }
    auto const good = elfC(
        "#include <sys/stat.h>\n"
        "int main(void){ struct stat st; if (stat(\".\", &st) != 0) return 1;\n"
        " if (fstat(0, &st) != 0) { }\n"
        " return (int)(st.st_mode != 0); }\n");
    EXPECT_FALSE(hasCode(good.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "the shape every real caller is written in must stay accepted";
    EXPECT_FALSE(good.diagnostics().hasErrors());
}

// The UCRT tag split, as a refusal. ✔MEASURED through the CLI on
// x86_64:pe64-x86_64-windows-exec before this fixture existed: passing a
// `struct stat *` to `_stat64` OR to `_stat64i32` is refused, and each
// underscore row accepts its own tag. That `_stat64i32` refuses `struct stat *`
// despite the IDENTICAL field list is the interesting half — it means two UCRT
// tags of the same layout stay distinct types, which is exactly what the UCRT's
// own casting forwarder says they are.
TEST(ShippedStatTypedSurface, ThePeUnderscoreRowsRefuseTheUserFacingTag) {
    for (char const* fn : {"_stat64", "_stat64i32"}) {
        std::string const src = std::string{"#include <sys/stat.h>\n"}
                                + "int main(void){ struct stat st; return "
                                + fn + "(\"x\", &st); }\n";
        auto const m = peC(src);
        EXPECT_TRUE(hasCode(m.diagnostics(), DiagnosticCode::S_TypeMismatch))
            << fn << " takes its OWN UCRT tag, not `struct stat *`";
    }
    auto const a = peC("#include <sys/stat.h>\n"
                       "int main(void){ struct _stat64 s; return _stat64(\"x\", &s); }\n");
    EXPECT_FALSE(a.diagnostics().hasErrors()) << "_stat64 with its own tag";
    auto const b = peC("#include <sys/stat.h>\n"
                       "int main(void){ struct _stat s; return _stat64i32(\"x\", &s); }\n");
    EXPECT_FALSE(b.diagnostics().hasErrors()) << "_stat64i32 with its own tag";
    auto const c = peC("#include <sys/stat.h>\n"
                       "int main(void){ struct stat s; return stat(\"x\", &s); }\n");
    EXPECT_FALSE(c.diagnostics().hasErrors())
        << "the user-facing pe row must still take `struct stat *`";
}

// The RETURN side, which the corpus example also witnesses by execution — kept
// here as well so the descriptor set is judged in one place.
TEST(ShippedStatTypedSurface, ATypedReturnNoLongerAssignsToAnyPointer) {
    auto const badTm = elfC("#include <time.h>\n"
                            "int main(void){ time_t t = 0; int *x = localtime(&t);"
                            " return x != 0; }\n");
    EXPECT_TRUE(hasCode(badTm.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "localtime yields `struct tm *`, not any pointer at all";
    auto const goodTm = elfC("#include <time.h>\n"
                             "int main(void){ time_t t = 0; struct tm *p = localtime(&t);"
                             " return p != 0; }\n");
    EXPECT_FALSE(goodTm.diagnostics().hasErrors());

    auto const badPw = elfC("#include <sys/types.h>\n#include <pwd.h>\n"
                            "int main(void){ int *x = getpwuid((uid_t)0);"
                            " return x != 0; }\n");
    EXPECT_TRUE(hasCode(badPw.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "getpwuid yields `struct passwd *`";
    auto const goodPw = elfC("#include <sys/types.h>\n#include <pwd.h>\n"
                             "int main(void){ struct passwd *p = getpwuid((uid_t)0);"
                             " return p != 0; }\n");
    EXPECT_FALSE(goodPw.diagnostics().hasErrors());
}

// ⚠ THE MATCHED CONTROL, PRINTED BY NAME. The same constraint violation through
// `<stdio.h>`, whose `FILE *` rows were typed over a struct pointer long before
// this row existed. If this one reds too, the pointer oracle moved and every
// descriptor above is innocent; if only the cases above red, those descriptors
// lost their types.
TEST(ShippedStatTypedSurface, TheFileControlDiagnosesTheSameShape) {
    auto const bad = elfC("#include <stdio.h>\n"
                          "int main(void){ FILE *f = fopen(\"x\", \"r\"); int *p;\n"
                          " if (f == 0) return 1; p = f; (void)p; fclose(f); return 0; }\n");
    EXPECT_TRUE(hasCode(bad.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "the control must diagnose, or it is not a control";
}

// ⚠⚠ THE OVER-REACH DETECTOR, AND IT IS THE MOST IMPORTANT TEST IN THIS FILE.
// The census's whole claim is that the defect was the INTERSECTION and not the
// `ptr<void>` count. These four arms are what makes that claim falsifiable:
//   • `void *` still converts in BOTH directions through the typed pointers
//     (C 6.3.2.3p1), which is what keeps sqlite's `os_unix.c` compiling;
//   • a NULL still reaches a typed pointer parameter (`utimes(path, 0)` is
//     exactly what `examples/c/shipped_utimes` does);
//   • `gettimeofday`'s `tz` — a REAL `void *` in glibc and the macOS SDK — must
//     still accept a pointer of any type, while its `tv` in the SAME CALL is
//     refused for the wrong type. One signature, two verdicts. ✔MEASURED
//     through the CLI: rc 0 and rc 1 respectively.
TEST(ShippedStatTypedSurface, TheGenuineVoidStarsWereLeftAloneAndStillAcceptAnything) {
    auto const viaVoid = elfC(
        "#include <sys/stat.h>\n"
        "int main(void){ struct stat st; void *p = &st; return stat(\".\", p); }\n");
    EXPECT_FALSE(viaVoid.diagnostics().hasErrors())
        << "void * -> T * is a standard implicit conversion and must stay one";

    auto const outToVoid = elfC(
        "#include <time.h>\n"
        "int main(void){ time_t t = 0; void *p = localtime(&t); return p != 0; }\n");
    EXPECT_FALSE(outToVoid.diagnostics().hasErrors())
        << "T * -> void * is the other direction of the same conversion";

    auto const nullArg = elfC(
        "#include <sys/time.h>\n"
        "int main(void){ return utimes(\"x\", 0) == -1; }\n");
    EXPECT_FALSE(nullArg.diagnostics().hasErrors())
        << "a null pointer constant converts to any pointer type; this is the "
           "shape examples/c/shipped_utimes is written in";

    auto const genuineTz = elfC(
        "#include <sys/time.h>\n"
        "int main(void){ struct timeval tv; int notATimezone = 0;\n"
        " return gettimeofday(&tv, &notATimezone); }\n");
    EXPECT_FALSE(genuineTz.diagnostics().hasErrors())
        << "glibc spells this parameter `void *__tz`; refusing it would be an "
           "invented extension, not a fix";

    auto const typedTv = elfC(
        "#include <sys/time.h>\n"
        "int main(void){ int notATimeval = 0; return gettimeofday(&notATimeval, 0); }\n");
    EXPECT_TRUE(hasCode(typedTv.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "the OTHER parameter of the SAME signature is typed — one call, two "
           "verdicts, which is the census applied per signature";
}

// The same two-verdict pair on the pe surface, where the split is starkest:
// `WriteFile`'s HANDLE and buffer are genuinely `void *` and take anything,
// while its `lpOverlapped` is now `OVERLAPPED *` — and a NULL still reaches it,
// which is what `examples/c/hello_writefile` depends on.
TEST(ShippedStatTypedSurface, TheWindowsSurfaceKeptItsRealVoidStars) {
    auto const genuine = peC(
        "#include <windows.h>\n"
        "int main(void){ unsigned long n = 0; int *anyptr = 0;\n"
        " void *h = GetStdHandle(0u); return WriteFile(h, anyptr, 1u, &n, 0); }\n");
    EXPECT_FALSE(genuine.diagnostics().hasErrors())
        << "LPCVOID takes any object pointer, and the NULL lpOverlapped is the "
           "shape hello_writefile is written in";

    auto const typedSlot = peC(
        "#include <windows.h>\n"
        "int main(void){ unsigned long n = 0; int bad = 0;\n"
        " void *h = GetStdHandle(0u); return WriteFile(h, \"x\", 1u, &n, &bad); }\n");
    EXPECT_TRUE(hasCode(typedSlot.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "lpOverlapped is `OVERLAPPED *` — one call, two verdicts again";

    auto const sync = peC(
        "#include <windows.h>\n"
        "int main(void){ SRWLOCK l; PSRWLOCK p = &l; InitializeSRWLock(p);\n"
        " AcquireSRWLockExclusive(p); ReleaseSRWLockExclusive(p); return 0; }\n");
    EXPECT_FALSE(sync.diagnostics().hasErrors())
        << "the retyped POINTER TYPEDEF must still be the type the API takes";

    auto const badSync = peC(
        "#include <windows.h>\n"
        "int main(void){ int bad = 0; InitializeSRWLock(&bad); return 0; }\n");
    EXPECT_TRUE(hasCode(badSync.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "an SRWLOCK slot is no longer any pointer at all";
}
