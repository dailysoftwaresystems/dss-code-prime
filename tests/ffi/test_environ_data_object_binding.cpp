// POSIX `environ` — the extern library DATA-OBJECT binding, and the three
// properties that make it correct rather than merely link-clean.
//
// WHY THIS TEST EXISTS
//
// ★★ THE SHAPE IS `environ` -> MACRO -> `__environ` -> DATA OBJECT.
//
// ⓘ READ THIS FIRST, BECAUSE THE ORIGINAL REASON FOR IT IS NOW HISTORY, AND A
// TEST DOCBLOCK THAT KEEPS ASSERTING A DEAD MECHANISM IS THE EXACT FAILURE MODE
// D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET IS ABOUT. When this file
// was written, ELF exec data imports bound by COPY RELOCATION, and under that
// mechanism the spelling was load-bearing for CORRECTNESS: a copy relocation
// redirects libc's references only for the name THE EXEC CLAIMS, so an `environ`
// row left glibc's startup writing its own slot while the exec read a copy
// filled before `__libc_start_main` ran — i.e. 0. That was SHIPPED and MEASURED
// (`readelf -r` showed `R_X86_64_COPY … environ + 0`, the dynsym showed
// `environ` as an 8-byte OBJECT, and the value read NULL on all four legs).
//
// ★ THAT MECHANISM IS GONE. Copy relocation was DELETED outright — claiming ONE
// name of glibc's `{environ, _environ, __environ}` alias set SPLIT the object in
// two, which broke any third-party `.so` reading the un-prefixed name and
// violated C23 6.2.2. Every ELF format now declares
// `dataImportBinding: "got-indirect"`: the exec DEFINES NOTHING and binds the
// library object's ADDRESS through a GOT slot, so ld.so resolves whichever
// spelling the descriptor names to the SAME one object. Under got-indirect
// EITHER spelling would therefore WORK.
//
// ★★ SO WHY DOES THIS FILE STILL PIN `__environ`? Two reasons that outlive the
// deleted mechanism, and NEITHER is "it would read NULL":
//   * `__environ` is glibc's UNCONDITIONALLY declared spelling
//     (/usr/include/unistd.h; plain `environ` sits behind `#ifdef __USE_GNU`)
//     and the STRONG (GLOBAL) export, while `environ`/`_environ` are WEAK. DSS
//     EAGER-IMPORTS every name a descriptor declares, so the safest choice is
//     the strong, always-present one.
//   * Declaring BOTH would emit TWO eager imports for ONE object, and an
//     `environ` SYMBOL row would collide with the `environ` MACRO this design
//     depends on (the macro wins at every call site, leaving the row
//     unreachable while still costing a load-time binding).
// The RUNTIME object-identity property itself is witnessed where it can only be
// witnessed — across an image boundary, by
// `examples/c-subset/environ_alias_object_identity`, which loads a gcc-built
// `.so` that reads the UN-PREFIXED name. A descriptor-shape test like this one
// cannot see identity at all, and must not claim to.
//
// So this file pins THREE properties, each of which fails silently if edited:
//
//   (1) THE SYMBOL IS `__environ`, NOT `environ` — the strong, unconditionally
//       declared spelling, declared ALONE.
//
//   (2) AVAILABILITY IS elf ONLY. DSS EAGER-IMPORTS every symbol a descriptor
//       DECLARES, not merely the ones a program calls
//       ([[D-FFI-DESCRIPTOR-EAGER-IMPORT]]), so widening the row to a format
//       whose runtime has no such export breaks the LOAD of EVERY binary that
//       so much as `#include`s the header — pe dies at 0xC0000139
//       STATUS_ENTRYPOINT_NOT_FOUND — with no link error and no diagnostic
//       naming the JSON. And the widening is a one-token edit. ✔MEASURED
//       (`objdump -p` plus a direct PE export-directory parse): ucrtbase.dll
//       (2,484 exports) exports NO `environ`, `_environ` or `__environ`;
//       msvcrt.dll (1,330) exports only the UNDERSCORED `_environ` data export
//       (ordinal 270). glibc exports all three names at ONE address on both run
//       legs — `__environ` STRONG (`B`/`g`), `environ`/`_environ` WEAK (`V`/`w`)
//       — 0x20ad58 @GLIBC_2.2.5 (x86_64) / 0x1b7288 @GLIBC_2.17 (aarch64),
//       `nm -D` and `objdump -T` concurring, `DO .bss` size 8.
//
//   (3) NO MACRO MAY SHADOW THE SYMBOL, AND NO SYMBOL MAY COLLIDE WITH THE
//       MACRO. When a descriptor realizes a MACRO and declares a SYMBOL of the
//       same name on the SAME format, the macro wins at every call site: the
//       compile succeeds, no diagnostic is produced, and the symbol row becomes
//       unreachable while its eager import is still emitted. For a DATA object
//       that is worse than for a function — these are LVALUES, so both reads AND
//       writes are silently redirected. Two directions to guard: a `__environ`
//       MACRO would shadow the row, and an `environ` SYMBOL would collide with
//       the macro this design depends on.
//
//       ⚠ SCOPE, DELIBERATELY NARROW: these are properties of THESE TWO NAMES,
//       not the general macro/symbol-overlap rule. That rule cannot be stated as
//       "same name + overlapping format = error", because tgmath.json
//       legitimately and standard-mandatorily does exactly that for 17 names
//       (C 7.25: the type-generic macro carries the SAME NAME as the <math.h>
//       function it dispatches to, and its replacement REFERENCES that name, so
//       the symbol is the macro's own callee and the import is not wasted — C
//       6.10.3.4p2 keeps the inner name unexpanded). Encoding a tree-wide rule
//       here would either red those 17 rows or require choosing a discriminator,
//       which is a design decision this test is not the place to make.
//
// It reads the REAL shipped descriptor tree (never a hand-copied snapshot) and
// is FAIL-CLOSED throughout: a missing repo root, a missing descriptor, an
// unparseable descriptor, an empty symbol array or an empty sweep all FAIL
// rather than vacuously pass.
//
// RED-ON-DISABLE: `WidenedAvailabilityIsCaught`, `WeakAliasSpellingIsCaught` and
// `ShadowingMacroIsCaught` reproduce each unsafe shape on SYNTHETIC descriptors,
// so every predicate stays pinned even while the real tree is correct — the
// test_pe_crt_costate_binding.cpp `SplitCoStateGroupIsCaught` idiom.

#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

[[nodiscard]] fs::path shippedLibsRoot() {
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return {};
    }
    return *root / "src" / "dss-config" / "shippedLibs";
}

// Load one descriptor, FAIL-CLOSED: the file must exist and parse.
[[nodiscard]] json loadDescriptor(fs::path const& p) {
    std::ifstream in(p);
    if (!in) {
        ADD_FAILURE() << "shipped descriptor unreadable: " << p.generic_string();
        return json::object();
    }
    json doc = json::object();
    try {
        in >> doc;
    } catch (std::exception const& e) {
        ADD_FAILURE() << "shipped descriptor " << p.generic_string()
                      << " does not parse: " << e.what()
                      << " -- a malformed descriptor breaks every #include of "
                         "that header";
        return json::object();
    }
    return doc;
}

// The availability set a SYMBOL row resolves to, mirroring the reader's
// two-level fallback (row key -> document key -> available everywhere).
// An EMPTY result means "every format", exactly as the reader encodes it.
[[nodiscard]] std::vector<std::string>
resolvedAvailability(json const& doc, json const& row) {
    std::vector<std::string> out;
    json const* src = nullptr;
    if (row.contains("availableObjectFormats")) {
        src = &row.at("availableObjectFormats");
    } else if (doc.contains("availableObjectFormats")) {
        src = &doc.at("availableObjectFormats");
    }
    if (src != nullptr && src->is_array()) {
        for (auto const& f : *src) {
            if (f.is_string()) out.push_back(f.get<std::string>());
        }
    }
    return out;
}

[[nodiscard]] bool availableOn(std::vector<std::string> const& set,
                               std::string_view fmt) {
    if (set.empty()) return true;  // empty == every format
    for (auto const& f : set) {
        if (f == fmt) return true;
    }
    return false;
}

[[nodiscard]] json const* findNamed(json const& doc, char const* section,
                                    std::string_view name) {
    if (!doc.contains(section) || !doc.at(section).is_array()) return nullptr;
    for (auto const& e : doc.at(section)) {
        if (e.is_object() && e.contains("name") && e.at("name").is_string()
            && e.at("name").get<std::string>() == name) {
            return &e;
        }
    }
    return nullptr;
}

// THE GUARD PREDICATES, factored so the synthetic red-on-disable tests below
// exercise the SAME code the real-tree assertions run.

// (1) A data-object row may only be available on formats whose runtime exports
//     the name. `allowed` is the measured-safe set.
[[nodiscard]] bool availabilityIsSafe(json const& doc, json const& row,
                                      std::vector<std::string> const& allowed) {
    auto const set = resolvedAvailability(doc, row);
    if (set.empty()) return false;  // "every format" is never safe for this name
    for (auto const& f : set) {
        bool ok = false;
        for (auto const& a : allowed) {
            if (f == a) ok = true;
        }
        if (!ok) return false;
    }
    return true;
}

// (2) The environment data object must be bound under the STRONG glibc alias,
//     and under it ALONE (see the header: the strong+unconditional export, one
//     eager import, no collision with the `environ` macro).
[[nodiscard]] bool bindsStrongEnvironAlias(json const& doc) {
    if (findNamed(doc, "symbols", "__environ") == nullptr) return false;
    return findNamed(doc, "symbols", "environ") == nullptr
           && findNamed(doc, "symbols", "_environ") == nullptr;
}

// (3) A name is macro-realized in this descriptor.
[[nodiscard]] bool hasMacroNamed(json const& doc, std::string_view name) {
    return findNamed(doc, "macros", name) != nullptr;
}

}  // namespace

// The real rows: the strong-alias data object plus the POSIX-spelling macro.
TEST(EnvironDataObjectBinding, RealUnistdJsonEnvironElfOnlyDataObject) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "unistd.json";
    ASSERT_TRUE(fs::exists(path)) << "unistd.json missing: " << path.generic_string();

    json const doc = loadDescriptor(path);
    ASSERT_TRUE(doc.contains("symbols")) << "unistd.json declares no symbols";
    ASSERT_TRUE(doc.at("symbols").is_array());
    ASSERT_FALSE(doc.at("symbols").empty())
        << "fail-closed: an empty symbols array would make every assertion "
           "below vacuous";

    // ── (1) THE STRONG ALIAS IS THE ONE BOUND ───────────────────────────────
    json const* row = findNamed(doc, "symbols", "__environ");
    ASSERT_NE(row, nullptr)
        << "unistd.json must declare `__environ` — glibc's UNCONDITIONALLY "
           "declared spelling (/usr/include/unistd.h; plain `environ` is behind "
           "#ifdef __USE_GNU) and the STRONG (GLOBAL) export, while "
           "`environ`/`_environ` are WEAK. DSS eager-imports every declared "
           "name, so the always-present one is the safe choice.";
    EXPECT_TRUE(bindsStrongEnvironAlias(doc))
        << "the environment object must be bound under `__environ` ONLY. "
           "Declaring a second spelling emits TWO eager imports for ONE object, "
           "and an `environ` SYMBOL row collides with the `environ` MACRO this "
           "design depends on — the macro wins at every call site, so the row "
           "is unreachable while still costing a load-time binding.";

    ASSERT_TRUE(row->contains("kind"));
    EXPECT_EQ(row->at("kind").get<std::string>(), "object")
        << "an extern DATA object; declared as a function it would bind a "
           "callable import thunk and every read would return code bytes";
    ASSERT_TRUE(row->contains("linkage"));
    EXPECT_EQ(row->at("linkage").get<std::string>(), "external");

    // `char **` — the ptr<ptr<char>> spelling strtoll's endptr already uses. A
    // single-level ptr would make environ[0] read a char, not a string.
    ASSERT_TRUE(row->contains("signature"));
    EXPECT_EQ(row->at("signature").get<std::string>(), "ptr<ptr<char>>")
        << "`char **`; any other pointer depth silently reshapes every "
           "environ[i] access";

    // ── (2) AVAILABILITY: elf ONLY. Measured — see the file header ───────────
    auto const set = resolvedAvailability(doc, *row);
    EXPECT_TRUE(availableOn(set, "elf"))
        << "glibc exports `__environ` STRONG on both run legs";
    EXPECT_FALSE(availableOn(set, "pe"))
        << "NEITHER Windows CRT exports any `environ` spelling ucrtbase can "
           "bind (ucrtbase: none at all; msvcrt: only `_environ`), so a pe arm "
           "would break the LOAD of every binary including <unistd.h>";
    EXPECT_FALSE(availableOn(set, "macho"))
        << "the macho export is UNMEASURED this cycle; under the eager-import "
           "law an inferred row risks every macho binary's LOAD, so macho stays "
           "a fail-loud S0001 until a consumer lands and the export is measured";
    EXPECT_TRUE(availabilityIsSafe(doc, *row, {"elf"}));

    // ── (3) THE POSIX SPELLING IS A MACRO ONTO THE STRONG ALIAS ─────────────
    json const* macro = findNamed(doc, "macros", "environ");
    ASSERT_NE(macro, nullptr)
        << "`environ` must be macro-realized: POSIX.1 does not specify main's "
           "third parameter at all, so `environ` is the spelling portable code "
           "uses and it has to reach the strong alias";
    ASSERT_TRUE(macro->contains("variants"))
        << "the macro must be per-format `variants` (elf only), never a flat "
           "body: a flat macro is injected on EVERY format, so pe/macho sources "
           "would silently rewrite `environ` to a symbol they cannot bind";
    ASSERT_TRUE(macro->at("variants").is_array());
    ASSERT_EQ(macro->at("variants").size(), 1u)
        << "exactly one arm today (elf); a second arm means another format "
           "gained a binding whose export must be measured first";
    json const& arm = macro->at("variants").at(0);
    ASSERT_TRUE(arm.contains("when") && arm.at("when").contains("format"));
    EXPECT_EQ(arm.at("when").at("format").get<std::string>(), "elf");
    ASSERT_TRUE(arm.contains("replacement"));
    EXPECT_EQ(arm.at("replacement").get<std::string>(), "__environ")
        << "the macro must expand to the name the SYMBOL row declares -- "
           "expanding it to `environ` or `_environ` names a symbol no "
           "descriptor ships, so every use becomes an honest S0001";
    EXPECT_FALSE(arm.contains("params"))
        << "object-like: `environ` is an LVALUE, not a call";

    // Positive control: an ungated sibling really does read as "every format",
    // so the assertions above are discriminating rather than trivially true.
    json const* control = findNamed(doc, "symbols", "close");
    ASSERT_NE(control, nullptr) << "positive control: close must be present";
    EXPECT_FALSE(control->contains("availableObjectFormats"))
        << "positive control: close ships with no per-symbol availability set";
}

// Neither direction of the macro/symbol collision may exist anywhere in tree.
TEST(EnvironDataObjectBinding, NoDescriptorShadowsTheEnvironBinding) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";

    std::size_t scanned = 0;
    for (auto const& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        ++scanned;
        json const doc = loadDescriptor(e.path());
        std::string const where = e.path().filename().generic_string();

        // A `__environ` MACRO would shadow the data-object row at every call
        // site: compile rc=0, no diagnostic, the row unreachable, its eager
        // import still emitted -- and because this is an LVALUE, both reads and
        // writes are silently redirected.
        EXPECT_FALSE(hasMacroNamed(doc, "__environ"))
            << where << " declares a `macros` entry named `__environ`, which "
                        "would SHADOW the environment data-object row silently";

        // An `environ` SYMBOL would collide with the elf macro this design
        // depends on -- the macro would win and the import would be wasted.
        if (where != "unistd.json") {
            EXPECT_EQ(findNamed(doc, "symbols", "environ"), nullptr)
                << where << " declares an `environ` SYMBOL; on elf the "
                            "unistd.json macro shadows it, so the row would be "
                            "unreachable while still emitting an eager import";
        }
    }
    // FAIL-CLOSED: an empty sweep must not read as a pass.
    ASSERT_GT(scanned, 10u)
        << "only " << scanned << " descriptors scanned -- the sweep found "
           "almost nothing, so its silence means nothing";
}

// ── RED-ON-DISABLE: each unsafe shape, on synthetic descriptors ─────────────
//
// These keep the predicates pinned while the real tree is correct. Each builds
// the exact shape the real assertions forbid and asserts the predicate REJECTS
// it — so weakening a predicate to wave the real tree through reds here.

TEST(EnvironDataObjectBinding, WidenedAvailabilityIsCaught) {
    // The one-token edit: pe joins the set. No Windows CRT exports it.
    json const doc = json::parse(R"JSON({
      "header": "unistd.h",
      "availableObjectFormats": ["elf", "macho"],
      "symbols": [
        { "name": "__environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf", "pe"] }
      ]
    })JSON");
    json const* row = findNamed(doc, "symbols", "__environ");
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(availableOn(resolvedAvailability(doc, *row), "pe"))
        << "the synthetic really does declare pe (the shape under test)";
    EXPECT_FALSE(availabilityIsSafe(doc, *row, {"elf"}))
        << "the guard must REJECT a pe-widened environment row";

    // The DOCUMENT-level fallback is the same hole through another door: no row
    // key at all inherits [elf, macho], which includes macho.
    json const inherit = json::parse(R"JSON({
      "header": "unistd.h",
      "availableObjectFormats": ["elf", "macho"],
      "symbols": [
        { "name": "__environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external" }
      ]
    })JSON");
    json const* inheritRow = findNamed(inherit, "symbols", "__environ");
    ASSERT_NE(inheritRow, nullptr);
    EXPECT_FALSE(availabilityIsSafe(inherit, *inheritRow, {"elf"}))
        << "a row with no availability key INHERITS the document's set; the "
           "guard must not read that as elf-only";

    // And an entirely absent availability set means EVERY format.
    json const wide = json::parse(R"JSON({
      "header": "unistd.h",
      "symbols": [
        { "name": "__environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external" }
      ]
    })JSON");
    json const* wideRow = findNamed(wide, "symbols", "__environ");
    ASSERT_NE(wideRow, nullptr);
    EXPECT_TRUE(resolvedAvailability(wide, *wideRow).empty())
        << "no key anywhere == every format (the reader's encoding)";
    EXPECT_FALSE(availabilityIsSafe(wide, *wideRow, {"elf"}))
        << "the guard must REJECT an ungated environment row";
}

TEST(EnvironDataObjectBinding, WeakAliasSpellingIsCaught) {
    // The un-prefixed WEAK alias as the row. (Historically this was the MEASURED
    // silent-NULL shape under copy relocation; since that mechanism was deleted
    // it is no longer a correctness fault, but it is still the wrong choice —
    // a weak export where a strong, unconditionally declared one exists.)
    json const weak = json::parse(R"JSON({
      "header": "unistd.h",
      "availableObjectFormats": ["elf", "macho"],
      "symbols": [
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf"] }
      ]
    })JSON");
    EXPECT_NE(findNamed(weak, "symbols", "environ"), nullptr)
        << "the synthetic really does declare the weak spelling";
    EXPECT_FALSE(bindsStrongEnvironAlias(weak))
        << "the guard must REJECT a row bound to the weak `environ` alias -- "
           "and doubly so because that name is macro-realized on elf";

    // The other weak alias, same address in glibc, same failure.
    json const weak2 = json::parse(R"JSON({
      "header": "unistd.h",
      "symbols": [
        { "name": "_environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf"] }
      ]
    })JSON");
    EXPECT_FALSE(bindsStrongEnvironAlias(weak2))
        << "the guard must REJECT `_environ` too";

    // BOTH spellings present is also wrong: the weak row still emits its own
    // eager import and still offers the wrong binding to a consumer.
    json const both = json::parse(R"JSON({
      "header": "unistd.h",
      "symbols": [
        { "name": "__environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf"] },
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf"] }
      ]
    })JSON");
    EXPECT_FALSE(bindsStrongEnvironAlias(both))
        << "the guard must REJECT the strong row coexisting with a weak one";

    // Positive control: the real shape is ACCEPTED, so the predicate is not
    // simply always-false.
    json const good = json::parse(R"JSON({
      "header": "unistd.h",
      "symbols": [
        { "name": "__environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf"] }
      ]
    })JSON");
    EXPECT_TRUE(bindsStrongEnvironAlias(good))
        << "positive control: the strong-alias-only shape must be accepted";
}

TEST(EnvironDataObjectBinding, ShadowingMacroIsCaught) {
    json const doc = json::parse(R"JSON({
      "header": "unistd.h",
      "availableObjectFormats": ["elf", "macho"],
      "macros": [
        { "name": "__environ", "variants": [
          { "when": { "format": "elf" }, "replacement": "(*envAccessor())" }
        ] }
      ],
      "symbols": [
        { "name": "__environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf"] }
      ]
    })JSON");
    EXPECT_TRUE(hasMacroNamed(doc, "__environ"))
        << "the guard must DETECT a macro shadowing the data-object row";

    // And it must not fire on an unrelated accessor macro (no false positive).
    json const benign = json::parse(R"JSON({
      "header": "stdio.h",
      "macros": [ { "name": "stdin", "variants": [
        { "when": { "format": "pe" }, "replacement": "(__acrt_iob_func(0))" }
      ] } ],
      "symbols": []
    })JSON");
    EXPECT_FALSE(hasMacroNamed(benign, "__environ"))
        << "the guard must not fire on an unrelated accessor macro";
}
