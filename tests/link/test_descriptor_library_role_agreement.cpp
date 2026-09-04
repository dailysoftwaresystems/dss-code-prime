// D-CONFIG-DESCRIPTOR-LIBRARY-LITERAL-DUPLICATES-THE-FORMAT-ROLE-TABLE —
// THE TWO OWNERS OF "WHICH IMAGE DOES THIS FORMAT FAMILY IMPORT ITS RUNTIME
// FROM", MADE TO AGREE OR FAIL LOUD.
//
// WHY THIS FILE EXISTS
//
// An object format's `runtimeLibraries` table is the declared SINGLE OWNER of
// role → image, and `resolveRuntimeRole` enforces that ownership INSIDE the
// format document: a spine block there may name a ROLE and may not name a
// literal image, because a literal "would be a second owner of a fact the role
// table owns".
//
// One layer down, `src/dss-config/shippedLibs/*.json` names the SAME images as
// LITERALS, per object-format family — and nothing whatsoever forces the two
// tiers to agree. ✔MEASURED at this commit over the shipped corpus: 49
// descriptor files, 31 of which carry a `library` map, 69 (descriptor, format)
// entries in total. 16 pe entries name `ucrtbase.dll` (the pe `cLibrary`
// image), 23 elf entries name `libc.so.6` (the elf `cLibrary` image), 26 macho
// entries name `/usr/lib/libSystem.B.dylib` (the macho `cLibrary` image), 2 pe
// entries name `kernel32.dll` (the pe `systemPrimitives` image) — so 67 of the
// 69 are a second spelling of a role's image — and 2 elf entries name
// `libm.so.6`, which plays no role at all.
//
// ★★ THE FAILURE THIS GUARDS IS SILENT AT EVERY COMPILE STAGE, WHICH IS WHY IT
// IS WORTH A FILE. Repointing a `cLibrary` row is a ONE-LINE config edit whose
// effect is real and already pinned — `RuntimeLibraryRoles.
// RepointingCLibraryChangesTheEmittedImportTable` shows the emitted pe import
// table following the row. What NOTHING showed until this file is that the
// descriptors do NOT follow it. The build after such an edit is rc=0 at every
// stage and emits a binary importing `exit` from the NEW image (the format's
// `processExit` block, which resolves the role) and `printf` from the OLD one
// (stdlib/stdio.json, which spell the literal). DSS EAGER-IMPORTS every function
// a descriptor lists, so an image that does not export them is a LOAD failure
// for every binary — pe 0xC0000139, macho exit 127 — with no diagnostic naming
// any JSON line. ⓘ `exit` is the sharpest instance and it is not hypothetical:
// pe64/elf64/macho64 `-exec` all declare `processExit{role: cLibrary,
// importMangledName: "exit"}` while `stdlib.json` ALSO declares `exit` with a
// literal image, so ONE symbol's import image is written twice, in two
// documents, on all three families.
//
// ⚠⚠ WHAT THIS FILE IS NOT. It is NOT the closing of that anchor. The anchor's
// closing work is to give the descriptor `library` value a `role` alternative so
// the literal disappears and there is ONE owner; that requires the format's
// resolved role table to reach `readShippedLibDescriptor`, which is called only
// from `src/analysis/semantic/semantic_analyzer.cpp` — a tier that holds an
// `ObjectFormatKind` and no schema. Until that is threaded, the two owners
// remain and the most that can be done is to make a divergence LOUD. That is
// what this is: agreement enforced, not ownership merged.
//
// ⚠ AND THE MIGRATION HAS A SECOND PRECONDITION THIS FILE MEASURES ON THE WAY
// PAST IT. ✔MEASURED: only **7 of the 24** shipped format documents declare a
// `cLibrary` row at all — the four `-exec` flavours and the two elf `-pie` ones
// — and only `pe64-x86_64-windows-exec` declares `systemPrimitives`. The 17
// that do not include every `-staticlib`, `-dyn`, `-dll` and `-dylib` flavour,
// all of which are live build targets. A role reference resolved against the
// ACTIVE format document, refusing loud when the role is undeclared (which is
// the correct refusal — never a fallback image), would therefore turn a working
// static-library or DLL build of any program that includes a C header into a
// hard failure the day the first descriptor is migrated. The rows have to be
// declared on those flavours FIRST, or the resolution has to be keyed on the
// format FAMILY, which is the tier the role fact actually belongs to
// (`RuntimeLibraryRoles.EveryFlavourOfAFormatKindNamesOneProviderPerRole`
// already says so in as many words) and the tier that has no document.
//
// ── THE VERDICT TABLE, AND WHY IT IS (KIND, IMAGE) AND NOT (DESCRIPTOR, KIND) ─
//
// The invariant needs to be ROLE-SPECIFIC, and an image alone cannot say which
// role it plays: ✔MEASURED, `ucrtbase.dll` is BOTH the pe `cLibrary` and the pe
// `unwindPersonality` image, and `/usr/lib/libSystem.B.dylib` is BOTH the macho
// `cLibrary` and the macho `atomicsRuntime` image. A guard that only asked "is
// this image SOME declared role's image" would stay GREEN over a `cLibrary`
// repoint, because the old spelling would still be `unwindPersonality`'s — a
// true answer to an adjacent question, failing toward clean.
//
// So each (kind, image) pair the corpus uses carries a verdict naming the role
// that OWNS it, and the assertion is that the role table's image for that role
// is exactly this spelling. Keying on (kind, image) rather than on every one of
// the 69 (descriptor, kind) entries is not a shortcut: ✔MEASURED, the intent is
// uniform per pair — every pe descriptor naming `ucrtbase.dll` is a C-library
// header, and both descriptors naming `kernel32.dll` (`windows.json`'s SRW /
// CRITICAL_SECTION surface and `threads.json`'s C11 threads, which the win32
// `librarySynthesis` vehicle emits over) are the OS-primitive image by the
// role's own definition. A pair whose descriptors ever disagree about intent
// would have to be split, and the enumeration below is what forces that to be
// noticed rather than absorbed.
//
// ★ THE `libm.so.6` ROW IS THE ESCAPE, AND IT IS DIRECTIONAL. It is the one
// image the corpus names that plays NO role, and it is why the literal channel
// cannot simply be deleted. Its arm asserts the OPPOSITE of every other row —
// that no elf role names it — so the escape cannot silently widen into "and
// anything else we forget to classify". Both directions of the enumeration are
// asserted, so an unused verdict row fails exactly as loudly as an unclassified
// image: an escape every row triggers is an escape that refuses nothing.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::ffi::readShippedLibDescriptor;

namespace {

// One (object-format kind, image spelling) pair the shipped descriptor corpus
// names, and the `runtimeLibraries` role that OWNS that image. An EMPTY role is
// the stated exception: an image the role table does not name at all.
struct LibraryImageVerdict {
    std::string_view format;   // the object-format KIND name a `library` key uses
    std::string_view image;    // the spelling the descriptors write
    std::string_view role;     // the owning role; EMPTY ⇒ owned by no role
    std::string_view why;      // the argument, restated when the verdict moves
};

inline constexpr std::array<LibraryImageVerdict, 5> kVerdicts{{
    {"pe", "ucrtbase.dll", "cLibrary",
     "the UCRT is the pe C library — the image that owns exit, the stdio family "
     "and the CRT argument machinery, which is the `cLibrary` role's own "
     "definition. Every pe descriptor naming it is a C-library header."},
    {"pe", "kernel32.dll", "systemPrimitives",
     "the OS primitive image. `windows.json` declares the SRWLock / "
     "CRITICAL_SECTION surface and `threads.json` declares C11 threads, which "
     "the pe `librarySynthesis` win32 vehicle emits over — the `systemPrimitives` "
     "role's own definition, and the role that block already names."},
    {"elf", "libc.so.6", "cLibrary",
     "glibc is the elf C library. ⚠ NOT `unwindPersonality`, which on elf is a "
     "DIFFERENT image (`libgcc_s.so.1`; libc exports no `_Unwind_*` at all), and "
     "not `atomicsRuntime` (`libatomic.so.1`) — the elf table is the one that "
     "proves a single per-format CRT string could never have modelled this."},
    {"elf", "libm.so.6", "",
     "★ THE STATED EXCEPTION. `math.json` and `tgmath.json` import from the "
     "separate glibc math object, which plays NO runtime role: no spine block "
     "needs it, so nothing declares it, so a role reference could not resolve. "
     "This is the entry that makes the literal channel permanent rather than "
     "transitional — the fix for the duplication ADDS a role alternative beside "
     "literals and never removes literals."},
    {"macho", "/usr/lib/libSystem.B.dylib", "cLibrary",
     "the darwin umbrella library is the macho C library. It ALSO plays "
     "`atomicsRuntime` on this family — the sameness the role table permits "
     "without asserting — which is exactly why the verdict has to name a role "
     "instead of letting the image speak for itself."},
}};

[[nodiscard]] fs::path configRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg;
}

// ── SIDE A: the role tables, read through THEIR OWN LOADER ────────────────
//
// `ObjectFormatSchema::loadShipped`, never a raw JSON read of `runtimeLibraries`
// — a second reader of a key the `link` tier owns is how a sweep and a loader
// come to disagree, and this file's whole subject is two readers disagreeing.
//
// The result is keyed on the format KIND because that is what a descriptor's
// `library` map is keyed on. Who plays a role is a property of the FAMILY (the
// rule `RuntimeLibraryRoles.EveryFlavourOfAFormatKindNamesOneProviderPerRole`
// already enforces across flavours); this tier has no family document, so the
// value is collected from every flavour that declares the row and a disagreement
// is reported here rather than silently resolved by iteration order.
struct RoleTables {
    // (kind, role name) -> image spelling. Roles filled by a shipped SOURCE
    // rather than an image are recorded with an EMPTY image: declaredness is a
    // ROW question, never an image question.
    std::map<std::pair<std::string, std::string>, std::string> image;
    // (kind, role name) -> the documents that declared it, for the diagnostics.
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> from;
    std::size_t documents = 0;
};

[[nodiscard]] RoleTables loadRoleTables() {
    RoleTables out;
    auto const root = configRoot();
    if (root.empty()) return out;
    fs::path const  dir = root / "object-formats";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        ADD_FAILURE() << "object-formats directory not found at " << dir;
        return out;
    }
    constexpr std::string_view kSuffix = ".format.json";
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string const leaf = it->path().filename().generic_string();
        if (leaf.size() <= kSuffix.size()) continue;
        if (leaf.compare(leaf.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        std::string const name = leaf.substr(0, leaf.size() - kSuffix.size());
        auto loaded = ObjectFormatSchema::loadShipped(name);
        if (!loaded.has_value()) {
            ADD_FAILURE() << name << " must load; a document this sweep cannot "
                             "read is a hole in it, never a pass";
            continue;
        }
        ++out.documents;
        std::string const kind{objectFormatKindName((*loaded)->kind())};
        for (auto const& b : (*loaded)->runtimeLibraries().bindings) {
            std::pair<std::string, std::string> const key{
                kind, std::string{runtimeLibraryRoleName(b.role)}};
            auto const found = out.image.find(key);
            if (found == out.image.end()) {
                out.image.emplace(key, b.image);
            } else {
                EXPECT_EQ(found->second, b.image)
                    << "format kind '" << kind << "' role '" << key.second
                    << "' is declared with two different images across its "
                       "flavour documents; this sweep would then compare the "
                       "descriptors against whichever one it happened to read "
                       "first";
            }
            out.from[key].push_back(name);
        }
    }
    return out;
}

// ── SIDE B: the descriptor corpus, read through ITS OWN LOADER ────────────
//
// `readShippedLibDescriptor`, for the same reason side A uses the format loader:
// the per-symbol `library` OVERRIDE and the descriptor-level map share one
// decode chokepoint, and a raw JSON walk here would be a third reading of a
// grammar this file exists to stop from forking. The read is done with NO active
// format so the WHOLE map decodes — every format key, not only the one a
// particular build selects. An arm no current target selects must not rot.
struct DescriptorImage {
    std::string descriptor;   // config-root-relative, forward slashes
    std::string context;      // "(root)" or "symbols['name']"
    std::string format;       // the `library` map key = an object-format KIND name
    std::string image;
};

[[nodiscard]] std::vector<DescriptorImage> loadDescriptorImages(
    std::size_t& descriptorsRead) {
    std::vector<DescriptorImage> out;
    descriptorsRead = 0;
    auto const root = configRoot();
    if (root.empty()) return out;
    fs::path const  dir = root / "shippedLibs";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        ADD_FAILURE() << "shippedLibs directory not found at " << dir;
        return out;
    }

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    // stdio.json's `vfprintf` spells the ABI alias `va_list`; without a binding
    // the read fails loud. Any consistent stand-in works — nothing here reads a
    // TypeId (the `test_shipped_type_consistency` precedent).
    std::array<NamedTypeBinding, 1> const named{
        NamedTypeBinding{"va_list",
                         interner.pointer(interner.primitive(TypeKind::Void))}};

    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it{dir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".json") continue;
        paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());

    for (auto const& path : paths) {
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                             DataModel::Lp64,
                                             /*activeTarget=*/std::nullopt,
                                             /*activeFormat=*/std::nullopt,
                                             named);
        // A descriptor that does not READ is a different invariant (pinned by
        // test_shipped_lib_descriptor) — but it MUST NOT silently shrink this
        // sweep, so it is surfaced rather than skipped in silence.
        EXPECT_TRUE(desc.has_value())
            << path.filename().generic_string() << " failed to read";
        if (!desc.has_value()) continue;
        ++descriptorsRead;
        std::string const rel =
            fs::relative(path, dir, ec).generic_string();
        for (auto const& [fmt, img] : desc->library)
            out.push_back(DescriptorImage{rel, "(root)", fmt, img});
        for (auto const& sym : desc->symbols) {
            for (auto const& [fmt, img] : sym.library) {
                out.push_back(DescriptorImage{
                    rel, "symbols['" + sym.name + "']", fmt, img});
            }
        }
    }
    return out;
}

[[nodiscard]] LibraryImageVerdict const* verdictFor(std::string_view format,
                                                    std::string_view image) {
    for (auto const& v : kVerdicts)
        if (v.format == format && v.image == image) return &v;
    return nullptr;
}

}  // namespace

// ── (1) THE ENUMERATION, BOTH DIRECTIONS ──────────────────────────────────
//
// Every (kind, image) pair the corpus uses must carry a verdict, and every
// verdict must be USED. The second half is the one that ratchets: a verdict row
// nobody triggers is a classification of a configuration that no longer exists,
// and leaving it is how an exception list becomes a place things are added to.
TEST(DescriptorLibraryRoleAgreement, EveryImageTheCorpusNamesCarriesAVerdict) {
    std::size_t descriptorsRead = 0;
    auto const  entries = loadDescriptorImages(descriptorsRead);
    ASSERT_FALSE(entries.empty())
        << "no descriptor named any library image — every assertion in this "
           "file would be vacuous";

    std::set<std::pair<std::string, std::string>> used;
    for (auto const& e : entries) {
        used.emplace(e.format, e.image);
        EXPECT_NE(verdictFor(e.format, e.image), nullptr)
            << e.descriptor << " " << e.context << ": `library." << e.format
            << "` names '" << e.image
            << "', which this file carries no verdict for. Every image the "
               "shipped corpus names is either owned by a `runtimeLibraries` "
               "role — in which case the two tiers must agree and the verdict "
               "says which role — or is a stated exception owned by no role. An "
               "unclassified image is an unreviewed second owner.";
    }

    for (auto const& v : kVerdicts) {
        EXPECT_NE(used.find({std::string{v.format}, std::string{v.image}}),
                  used.end())
            << "the verdict for (" << v.format << ", " << v.image
            << ") is never triggered: no shipped descriptor names that image on "
               "that object format any more. Delete the row — a classification "
               "kept past the configuration it classified is how an exception "
               "list stops refusing anything.";
    }

    // ⚠ NON-VACUITY. Each count closes a different way this sweep could pass
    // having compared nothing: the corpus was read at all, the entries are the
    // measured population rather than a handful, and all three object-format
    // families with a C runtime are represented.
    EXPECT_GE(descriptorsRead, 40u)
        << "only " << descriptorsRead << " descriptor(s) read";
    EXPECT_GE(entries.size(), 60u)
        << "only " << entries.size() << " library entr(ies) collected";
    std::set<std::string> kinds;
    for (auto const& e : entries) kinds.insert(e.format);
    EXPECT_GE(kinds.size(), 3u)
        << "only " << kinds.size() << " object-format kind(s) covered";
}

// ── (2) THE AGREEMENT ─────────────────────────────────────────────────────
//
// THE LEVER. For every descriptor entry whose verdict names a role, the role
// table's image for that role must be exactly the spelling the descriptor
// writes. Repointing a role's image without repointing its descriptors reds
// here, naming both sides — which is the divergence that is otherwise silent
// through every compile stage and surfaces as a LOAD failure.
TEST(DescriptorLibraryRoleAgreement, EveryRoleOwnedImageEqualsTheRoleTablesImage) {
    auto const  tables = loadRoleTables();
    std::size_t descriptorsRead = 0;
    auto const  entries = loadDescriptorImages(descriptorsRead);
    ASSERT_GE(tables.documents, 20u)
        << "the shipped object-format corpus must have been walked; only "
        << tables.documents << " document(s) loaded";
    ASSERT_FALSE(entries.empty());

    std::size_t compared = 0;
    std::set<std::string> rolesCompared;
    for (auto const& e : entries) {
        auto const* const v = verdictFor(e.format, e.image);
        if (v == nullptr) continue;         // (1) already reported it
        if (v->role.empty()) continue;      // the stated exception — arm (3)
        std::pair<std::string, std::string> const key{e.format,
                                                      std::string{v->role}};
        auto const found = tables.image.find(key);
        if (found == tables.image.end()) {
            ADD_FAILURE()
                << e.descriptor << " " << e.context << ": `library." << e.format
                << "` is classified as the '" << v->role
                << "' image, but NO shipped " << e.format
                << " format document declares that role. Either the role row was "
                   "deleted and these descriptors are now the only owner of the "
                   "image, or the verdict is wrong. " << v->why;
            continue;
        }
        ++compared;
        rolesCompared.insert(std::string{v->role});
        EXPECT_EQ(found->second, e.image)
            << e.descriptor << " " << e.context << ": `library." << e.format
            << "` names '" << e.image << "' but the " << e.format
            << " `runtimeLibraries` role '" << v->role << "' names '"
            << found->second << "'. These are two owners of ONE fact — which "
               "image this format family imports its runtime from — and they "
               "have diverged. The format's own spine blocks resolve the ROLE, "
               "so they follow the table; these descriptors spell a LITERAL, so "
               "they do not. A build in this state is rc=0 at every stage and "
               "produces a binary importing some symbols from one image and the "
               "rest from another, which the loader refuses at process start "
               "(pe 0xC0000139 / macho 127) with no diagnostic naming any JSON "
               "line. Repoint the descriptors too, or revert the role.";
    }

    // ⚠ NON-VACUITY, and the second count is the one that matters: a guard that
    // exercised ONE role would pass while every other role's descriptors rotted.
    EXPECT_GE(compared, 60u)
        << "only " << compared << " entr(ies) were actually compared against a "
                                  "role table";
    EXPECT_GE(rolesCompared.size(), 2u)
        << "only " << rolesCompared.size()
        << " distinct role(s) were exercised; this guard is written for a corpus "
           "that binds more than one, and a drop to one means the rows for the "
           "others stopped being compared";
}

// ── (3) THE ESCAPE, ASSERTED IN THE OPPOSITE DIRECTION ────────────────────
//
// An image classified as owned by NO role must be owned by no role — checked
// against every role the family declares, not merely against the one a reader
// had in mind. Without this arm the empty-role verdict would be an escape that
// any future image could be added to, and (2) would skip it silently.
TEST(DescriptorLibraryRoleAgreement, AnImageStatedToPlayNoRolePlaysNone) {
    auto const tables = loadRoleTables();
    ASSERT_GE(tables.documents, 20u);

    std::size_t checked = 0;
    for (auto const& v : kVerdicts) {
        if (!v.role.empty()) continue;
        ++checked;
        for (auto const& [key, image] : tables.image) {
            if (key.first != v.format) continue;
            EXPECT_NE(image, std::string{v.image})
                << "(" << v.format << ", " << v.image
                << ") is classified as playing NO runtime role, but the '"
                << key.second
                << "' role now names exactly that image. The exception has "
                   "become a duplicate: the descriptors and the role table are "
                   "two owners of it, and nothing else in this file will catch "
                   "the divergence because arm (2) skips this pair. Reclassify "
                   "it with the role it now plays.";
        }
    }
    EXPECT_GE(checked, 1u)
        << "no verdict claims an image plays no role, so this arm compared "
           "nothing — the `libm.so.6` row it exists for has gone, and its "
           "disappearance must be argued, not absorbed";
}
