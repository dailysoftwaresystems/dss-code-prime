// ── [[D-FFI-DIRENT-API-DECLARED-OVER-VOID-NOT-ITS-OWN-STRUCTS]] ─────────────
//
// THE DEFECT. `dirent.json` declared `opendir :: fn(ptr<char>) -> ptr<void>`,
// `readdir :: fn(ptr<void>) -> ptr<void>` and `closedir :: fn(ptr<void>) -> i32`
// while the SAME descriptor already declared, correctly and per object format,
// BOTH of the types those three signatures are about: an opaque `DIR` typedef
// and a `struct dirent` carrying elf / pe / macho variants. So the mechanism
// whose entire purpose is to TYPE a shipped library's surface asserted nothing
// about this one. `int *x = readdir(d);` compiled clean here; POSIX, glibc and
// mingw-w64 all spell the row `struct dirent *readdir(DIR *)` and all three
// references diagnose it.
//
// ★★ WHY IT NEEDED A READER CHANGE AND NOT JUST A CONFIG EDIT. A `signature`
// resolves named types out of `mergedNamedTypes`, and until P56 the STRUCT
// surface was the one named-type surface still decoded AFTER `symbols` —
// typedefs and unions had each been relocated ahead of it in earlier cycles for
// exactly this reason. `ptr<dirent>` was therefore not a spellable type at all,
// which is ✔MEASURED to be why NOT ONE signature in the whole shipped corpus
// referenced a struct its own descriptor declared. The alternative — restating
// the layout INLINE in the signature — is a fresh defect wearing a fix's
// clothes: `struct dirent` is 280 bytes on glibc, 268 on pe and 1048 on Darwin,
// so one inline body would be wrong on two targets out of three.
//
// ★ THE THREE MEASURED LAYOUTS THIS FILE PINS (2026-09-03, all by execution):
//     elf x86_64 / aarch64 (glibc, cross-checked native + qemu):
//         280 B, align 8 — d_ino u64@0, d_off i64@8, d_reclen u16@16,
//         d_type u8@18, d_name char[256]@19  (IDENTICAL on both arches)
//     pe (mingw-w64 gcc 13.2.0, native, LLP64):
//         268 B, align 4 — d_ino i32@0, d_reclen u16@4, d_namlen u16@6,
//         d_name char[260]@8
//     macho arm64 (Darwin 64-bit-inode): 1048 B, d_name@21
//   The pe row is the one DSS itself realizes: `runtime/platform/src/dirent.c`
//   includes this very header, so the layout agreement is checked by the
//   compiler on every pe build rather than by review.
//
// ⚠ WHAT THE CORPUS EXAMPLE CANNOT REACH, WHICH IS WHY THIS FILE EXISTS.
// `examples/c/shipped_dirent_typed_surface` proves the POSITIVE half by
// execution. The half that closes the row is a REFUSAL, and an example that
// must be refused is not a runnable example — so the refusals live here, each
// beside its nearest ACCEPTING twin, because a type check fails in two opposite
// directions and only the pair separates a fix that landed from one that
// over-reached.

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

[[nodiscard]] fs::path direntDescriptor() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg / "shippedLibs" / "dirent.json";
}

// One descriptor read, exactly as the semantic phase performs it for a target.
struct DirentRead {
    TypeInterner                       interner{CompilationUnitId{1}};
    TypeRegistry                       typeReg;
    DiagnosticReporter                 rep;
    std::optional<ShippedLibDescriptor> desc;

    DirentRead(std::string_view arch, std::optional<ObjectFormatKind> fmt) {
        fs::path const path = direntDescriptor();
        if (path.empty()) return;
        desc = readShippedLibDescriptor(
            path, interner, typeReg, rep, DataModel::Lp64,
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

    // The pointee of a function signature's RETURN type.
    [[nodiscard]] TypeId returnPointee(std::string_view name) const {
        auto const* s = symbol(name);
        if (s == nullptr || !s->signature.valid()) return TypeId{};
        TypeId const ret = interner.fnResult(s->signature);
        if (!ret.valid() || interner.kind(ret) != TypeKind::Ptr) return TypeId{};
        auto const ops = interner.operands(ret);
        return ops.empty() ? TypeId{} : ops[0];
    }

    // The pointee of parameter `i` of a function signature.
    [[nodiscard]] TypeId paramPointee(std::string_view name, std::size_t i) const {
        auto const* s = symbol(name);
        if (s == nullptr || !s->signature.valid()) return TypeId{};
        auto const params = interner.fnParams(s->signature);
        if (i >= params.size()) return TypeId{};
        TypeId const p = params[i];
        if (!p.valid() || interner.kind(p) != TypeKind::Ptr) return TypeId{};
        auto const ops = interner.operands(p);
        return ops.empty() ? TypeId{} : ops[0];
    }

    [[nodiscard]] TypeId structNamed(std::string_view name) const {
        if (!desc.has_value()) return TypeId{};
        for (auto const& s : desc->structs)
            if (s.name == name) return s.typeId;
        return TypeId{};
    }
};

}  // namespace

// ══ 1. THE DESCRIPTOR TIER — the signature is typed over the API's own types ══

// `readdir` returns a pointer to THE VERY struct this descriptor injects, not a
// look-alike: the TypeId the signature carries must be IDENTICAL to the one in
// `desc->structs`, because a distinct-but-similar type would resolve every
// member access through a second layout that could silently drift from the
// first. Asserted on all three served formats, whose layouts genuinely differ.
TEST(ShippedDirentTypedSurface, ReaddirReturnsTheDescriptorsOwnDirentPerFormat) {
    struct Case {
        ObjectFormatKind fmt;
        char const*      arch;
        std::uint64_t    size;
        std::size_t      dNameIndex;
        std::uint64_t    dNameOffset;
    };
    // MEASURED per target — see this file's header for how each was obtained.
    Case const cases[] = {
        {ObjectFormatKind::Elf,   "x86_64", 280u, 4u, 19u},
        {ObjectFormatKind::Elf,   "arm64",  280u, 4u, 19u},
        {ObjectFormatKind::Pe,    "x86_64", 268u, 3u,  8u},
        {ObjectFormatKind::MachO, "arm64", 1048u, 5u, 21u},
    };
    for (auto const& c : cases) {
        DirentRead r{c.arch, c.fmt};
        ASSERT_TRUE(r.desc.has_value()) << c.arch;
        EXPECT_FALSE(r.rep.hasErrors()) << c.arch;

        TypeId const injected = r.structNamed("dirent");
        ASSERT_TRUE(injected.valid()) << "no struct dirent injected for " << c.arch;

        TypeId const pointee = r.returnPointee("readdir");
        ASSERT_TRUE(pointee.valid())
            << "readdir's return type is not a pointer for " << c.arch;
        EXPECT_EQ(pointee, injected)
            << "readdir must return a pointer to the SAME struct dirent this "
               "descriptor injects, not a second one";
        EXPECT_NE(r.interner.kind(pointee), TypeKind::Void)
            << "the whole row: readdir must not be typed over void";

        // …and that ONE type carries the target's real layout, which is what
        // makes the by-name spelling correct rather than merely well-formed.
        auto const layout = computeLayout(pointee, r.interner, kNatural16,
                                          DataModel::Lp64);
        ASSERT_TRUE(layout.has_value()) << c.arch;
        EXPECT_EQ(layout->size, c.size) << c.arch;
        ASSERT_GT(layout->fieldOffsets.size(), c.dNameIndex) << c.arch;
        EXPECT_EQ(layout->fieldOffsets[c.dNameIndex], c.dNameOffset) << c.arch;
    }
}

// The handle half. `DIR` is the descriptor's OPAQUE typedef, so all three rows
// must name an INCOMPLETE composite — never void, and never a complete type
// (completing it here would hand user code a layout the platform does not
// guarantee and would break the TU-completes-the-tag case the corpus pins).
TEST(ShippedDirentTypedSurface, TheDirHandleIsTheOpaqueDirTagOnEveryRow) {
    for (auto fmt : {ObjectFormatKind::Elf, ObjectFormatKind::Pe,
                     ObjectFormatKind::MachO}) {
        DirentRead r{"x86_64", fmt};
        ASSERT_TRUE(r.desc.has_value());
        EXPECT_FALSE(r.rep.hasErrors());

        TypeId const opened   = r.returnPointee("opendir");
        TypeId const readParm = r.paramPointee("readdir", 0);
        TypeId const closeParm = r.paramPointee("closedir", 0);
        ASSERT_TRUE(opened.valid());
        ASSERT_TRUE(readParm.valid());
        ASSERT_TRUE(closeParm.valid());

        EXPECT_NE(r.interner.kind(opened), TypeKind::Void)
            << "opendir must not be typed over void";
        EXPECT_EQ(r.interner.kind(opened), TypeKind::Struct);
        EXPECT_EQ(r.interner.name(opened), "DIR")
            << "the handle must be the tag the descriptor's `DIR` typedef names, "
               "not some other incomplete struct that merely looks like one";
        EXPECT_TRUE(r.interner.isIncompleteComposite(opened))
            << "DIR must stay OPAQUE — a complete DIR would publish a layout no "
               "platform guarantees";
        // One handle type across the three rows: opendir's product IS what
        // readdir and closedir consume.
        EXPECT_EQ(opened, readParm);
        EXPECT_EQ(opened, closeParm);
    }
}

// ★★ A READ THAT SELECTS NO VARIANT TYPES THE TAG INCOMPLETE, AND DOES NOT FAIL.
// Two such reads exist and this pins BOTH, because they were once handled
// differently and the difference was a defect:
//   (a) the TARGET-LESS read — LSP, the direct API, the
//       `AllShippedDescriptorsDecode` sweep — which names no arch and no format;
//   (b) a read that DOES name a target `<dirent.h>` does not serve at all, which
//       is every non-{elf,pe,macho} object format the repo declares.
// In both, the tag EXISTS and only its LAYOUT is unknowable, so the tag is
// published INCOMPLETE: `ptr<dirent>` stays a well-formed pointer, no layout is
// injected, and any use needing a size or a field still fails loud.
//
// ⚠ ARM (b) IS HERE BECAUSE THE GATE REFUTED THE FIRST DESIGN, ON TWO LEGS AT
// ONCE. That design published only for (a), reasoning that a NAMED target with
// no variant was a genuine absence that should fail loud. It made `dirent.json`
// fail to READ on `wasm32-v1` and `spirv-1.6` — formats whose
// `availableObjectFormats` already says the header is not there — and took
// `ShippedTypeConsistency.EveryDescriptorAgreesOnEveryTagAndTypedefPerTarget`
// red on Windows and WSL alike.
//
// RED-ON-DISABLE: delete the no-variant publication in the reader's
// struct-selection block and both arms report `unknown type 'dirent'`.
TEST(ShippedDirentTypedSurface, AReadThatSelectsNoVariantTypesDirentIncomplete) {
    struct Arm { char const* what; char const* arch; std::optional<ObjectFormatKind> fmt; };
    Arm const arms[] = {
        {"target-less read", "", std::nullopt},
        {"a format <dirent.h> does not serve", "x86_64",
         std::optional<ObjectFormatKind>{ObjectFormatKind::Wasm}},
    };
    for (auto const& arm : arms) {
        DirentRead r{arm.arch, arm.fmt};
        ASSERT_TRUE(r.desc.has_value()) << arm.what;
        EXPECT_FALSE(r.rep.hasErrors())
            << arm.what << ": a read that cannot state a layout must not fail";
        EXPECT_EQ(r.desc->symbols.size(), 3u)
            << arm.what << ": no symbol may be dropped";

        // No LAYOUT is published — `out.structs` is byte-identical to before.
        EXPECT_TRUE(r.desc->structs.empty())
            << arm.what << ": no variant matched, so no layout may be injected";

        TypeId const pointee = r.returnPointee("readdir");
        ASSERT_TRUE(pointee.valid()) << arm.what;
        EXPECT_EQ(r.interner.kind(pointee), TypeKind::Struct) << arm.what;
        EXPECT_TRUE(r.interner.isIncompleteComposite(pointee)) << arm.what;
        EXPECT_EQ(r.interner.name(pointee), "dirent") << arm.what;
    }
}

// ══ 2. THE SEMANTIC TIER — the refusals the row exists for, each with a twin ══

namespace {

// The elf x86_64 leg, driven exactly as the production driver does, with the
// REAL shipped descriptor corpus on the system include path.
[[nodiscard]] SemanticModel analyzeDirent(std::string src, ObjectFormatKind fmt,
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

[[nodiscard]] SemanticModel analyzeDirentElf(std::string src) {
    return analyzeDirent(std::move(src), ObjectFormatKind::Elf, DataModel::Lp64,
                         "x86_64");
}

constexpr char const* kPrologue =
    "#include <dirent.h>\n"
    "int main(void){ DIR *d = opendir(\".\"); if (d == 0) return 1;\n";

[[nodiscard]] std::string program(char const* body) {
    return std::string{kPrologue} + body + "\n closedir(d); return 0; }\n";
}

}  // namespace

// ★★★ THE CLOSING TEST THE ROW NAMES. `int *x = readdir(d);` must be
// DIAGNOSED. Its twin — the same statement through the type the API actually
// returns — must stay accepted, or the claim is refusing everything rather than
// judging anything.
//
// ★ THE ASSERTION IS ON THE DIAGNOSTIC, NOT ON ITS SEVERITY, AND THAT IS A
//   MEASURED DECISION RATHER THAN A HEDGE. ✔MEASURED 2026-09-03, each reference
//   probed SEPARATELY on this exact program: gcc 13.3.0 (WSL, -std=c17 AND
//   -std=c2x), clang 18.1.3 (both), and mingw-w64 gcc 13.2.0 (both) ALL emit
//   `-Wincompatible-pointer-types` and ALL exit 0 — they DIAGNOSE it and COMPILE
//   it. DSS today answers `error[S_TypeMismatch]` and rc=1, which is stricter
//   than the union of the references. THAT STRICTNESS IS NOT THIS ROW'S AND IS
//   NOT NEW: ✔MEASURED against the UNMODIFIED tree, the matched control
//   `FILE *f; int *x; x = f;` — a header typed over a struct pointer since long
//   before P56 — is refused with the SAME code and the SAME rc there. So pinning
//   Error severity here would pin a divergence this lane did not introduce and
//   would red the day it is correctly relaxed. What the row claims, and what all
//   four toolchains agree on, is that the program must not pass in SILENCE.
TEST(ShippedDirentTypedSurface, ReaddirsResultNoLongerAssignsToAnyPointer) {
    auto const bad = analyzeDirentElf(program(" int *x = readdir(d); (void)x;"));
    EXPECT_TRUE(hasCode(bad.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "`int *x = readdir(d);` is a constraint violation every reference "
           "diagnoses; a void-typed row could not see it at all";

    auto const good =
        analyzeDirentElf(program(" struct dirent *e = readdir(d); (void)e;"));
    EXPECT_FALSE(hasCode(good.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "the ordinary shape every directory walk is written in must stay "
           "accepted";
    EXPECT_FALSE(good.diagnostics().hasErrors());
}

// ⚠ THE MATCHED CONTROL, PRINTED BY NAME. The same constraint violation through
// `<stdio.h>`, whose `FILE *` rows were typed over a struct pointer long before
// this row existed. It is here so a future reader can tell WHICH property a red
// in the case above belongs to: if this one reds too, the pointer-assignment
// oracle moved and the dirent rows are innocent; if only the case above reds,
// the dirent rows lost their types.
TEST(ShippedDirentTypedSurface, TheFileControlDiagnosesTheSameShape) {
    auto const bad = analyzeDirentElf(
        "#include <stdio.h>\n"
        "int main(void){ FILE *f = fopen(\"x\", \"r\"); int *p;\n"
        " if (f == 0) return 1; p = f; (void)p; fclose(f); return 0; }\n");
    EXPECT_TRUE(hasCode(bad.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "the control must diagnose, or it is not a control";
}

// The handle side of the same claim, and the reason `DIR` had to be threaded
// through rather than left implied: a directory handle is now a distinct type
// from an `int *`, so the classic transposition is caught at the declaration.
TEST(ShippedDirentTypedSurface, TheDirHandleIsNoLongerAnyPointer) {
    auto const bad = analyzeDirentElf(program(" int *h = opendir(\".\"); (void)h;"));
    EXPECT_TRUE(hasCode(bad.diagnostics(), DiagnosticCode::S_TypeMismatch))
        << "opendir's product is a DIR *, not any pointer at all";

    auto const good = analyzeDirentElf(program(" DIR *h = opendir(\".\"); (void)h;"));
    EXPECT_FALSE(hasCode(good.diagnostics(), DiagnosticCode::S_TypeMismatch));
    EXPECT_FALSE(good.diagnostics().hasErrors());
}

// ⚠ THE OVER-REACH DETECTOR. Typing the surface must NOT break the two shapes
// real code is written in: `void *` still converts to and from these pointers
// in both directions (C 6.3.2.3p1), which is what keeps sqlite's shell.c and
// `examples/c/pe_direct_dirent_superset` — which stages readdir's result
// through a `void *` — compiling. A fix that refused these would have replaced
// a laxness defect with a strictness one.
TEST(ShippedDirentTypedSurface, VoidPointerInteroperabilityIsUnchanged) {
    auto const viaVoid =
        analyzeDirentElf(program(" void *e = readdir(d); (void)e;"));
    EXPECT_FALSE(viaVoid.diagnostics().hasErrors())
        << "T * -> void * is a standard implicit conversion and must stay one";

    auto const backOut =
        analyzeDirentElf(program(" void *p = d; struct dirent *e = readdir(p);"
                                 " (void)e;"));
    EXPECT_FALSE(backOut.diagnostics().hasErrors())
        << "void * -> T * is the other direction of the same conversion";
}
