// D-CONFIG-DESCRIPTOR-LIBRARY-LITERAL-DUPLICATES-THE-FORMAT-ROLE-TABLE —
// ONE OWNER OF "WHICH IMAGE DOES THIS FORMAT FAMILY IMPORT ITS RUNTIME FROM",
// AND THE GUARD THAT KEEPS IT ONE.
//
// WHAT THIS FILE WAS, AND WHAT IT IS NOW
//
// An object format's `runtimeLibraries` table is the declared SINGLE OWNER of
// role → image, and `resolveRuntimeRole` enforces that ownership INSIDE the
// format document: a spine block there may name a ROLE and may not name a
// literal image. One tier down, `src/dss-config/shippedLibs/*.json` used to
// name the SAME images as LITERALS, per object-format family — ✔MEASURED at
// the commit before this migration: 49 descriptor files, 31 carrying a
// `library` map, 69 (descriptor, format) entries, 67 of which restated a
// declared role's image (16 pe `ucrtbase.dll` = `cLibrary`, 2 pe `kernel32.dll`
// = `systemPrimitives`, 23 elf `libc.so.6` = `cLibrary`, 26 macho
// `/usr/lib/libSystem.B.dylib` = `cLibrary`), and 2 elf entries naming
// `libm.so.6`, which plays no role at all. The first cut of this file made the
// two owners AGREE OR FAIL LOUD; it could not make them one, because the
// descriptor reader had no way to ask a format which image plays a role.
//
// It has one now. A `library` entry may spell `{"role": "cLibrary"}` and the
// reader resolves it through `RuntimeLibraryRoleResolver` (core) — the driver
// adapts the ACTIVE format to it with `FormatRuntimeLibraryRoleResolver`
// (link), which answers from the document's own row when it declares the role
// and otherwise from the shipped flavours of the same kind, which must agree.
// The 67 restatements are gone; the two `libm.so.6` literals stay, because an
// image that plays no role has no other spelling.
//
// ★★ WHAT IS LEFT TO GUARD, AND WHY EACH ARM EXISTS
//
//   (1) EVERY ROLE THE CORPUS REFERENCES RESOLVES, ON EVERY FLAVOUR OF ITS
//       FAMILY, TO ONE IMAGE — through the same adapter the driver uses, never
//       a private merge of the tables. This is what makes a `-staticlib` or
//       `-dll` build (whose own table declares no `cLibrary`) reach the family's
//       answer, and it is what a repoint of the family's row must move.
//   (2) THE RATCHET: no literal that REMAINS may restate any role's image on
//       its kind. A new descriptor spelling `"pe": "ucrtbase.dll"` is the
//       duplication this row ended, coming back one file at a time; it reds
//       here naming the role it should have named.
//   (3) THE ESCAPE, IN THE OPPOSITE DIRECTION: the literal population must be
//       exactly the stated exceptions (both directions of the enumeration), and
//       each exception must play NO role anywhere in its family — so the escape
//       cannot widen into "and anything else we forget to classify".
//   (4) THE DECODE ITSELF, OVER THE REAL CORPUS: reading every descriptor WITH
//       the adapter over EACH shipped flavour of a family it references yields,
//       in the plain-string `library` map every consumer reads, exactly the
//       image (1) established. A resolver that answered but a decode that
//       dropped the answer would pass (1) and ship an unbound import.
//
// ★ A REPOINT NOW FOLLOWS INSTEAD OF DIVERGING. The pre-migration arm "the
// role table's image equals the descriptor's literal" is gone by construction —
// there is no literal to compare — and its replacement is the driver-level
// witness in `tests/program/test_descriptor_role_follows_the_table.cpp`: a
// `cLibrary` repoint moves every role-bound import the linker receives, and
// the `libm.so.6` literal provably does not.
//
// Reads BOTH sides through their own loaders (`ObjectFormatSchema::loadShipped`,
// `readShippedLibDescriptor`) — a raw JSON walk here would be a third reading
// of a grammar this file exists to stop from forking. Lives in `tests/link/`
// because it loads format documents.

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
#include <memory>
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

// A LITERAL the corpus is allowed to keep: an image that plays NO runtime role
// on its family, so a role reference could not name it. Both directions of the
// enumeration are asserted below — an unclassified literal reds, and so does a
// classified one nothing writes any more.
struct LiteralException {
    std::string_view format;   // the object-format KIND name a `library` key uses
    std::string_view image;    // the spelling the descriptor writes
    std::string_view why;
};

inline constexpr std::array<LiteralException, 1> kLiteralExceptions{{
    {"elf", "libm.so.6",
     "`math.json` and `tgmath.json` import from the separate glibc math object, "
     "which plays NO runtime role: no spine block needs it, so nothing declares "
     "it, so a role reference could not resolve. This is the entry that makes "
     "the literal channel permanent rather than transitional."},
}};

[[nodiscard]] fs::path configRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg;
}

// ── SIDE A: every shipped flavour, grouped by its DECLARED kind ─────────────
//
// Read through `ObjectFormatSchema::loadShipped`, grouped by the kind the
// loaded schema reports — never a hardcoded {pe, elf, macho} list, which would
// silently stop covering the next kind the corpus grows.
using Flavours = std::map<std::string, std::vector<std::shared_ptr<ObjectFormatSchema>>>;

[[nodiscard]] Flavours loadFlavoursByKind(std::size_t& documents) {
    Flavours out;
    documents = 0;
    auto const root = configRoot();
    if (root.empty()) return out;
    fs::path const  dir = root / "object-formats";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        ADD_FAILURE() << "object-formats directory not found at " << dir;
        return out;
    }
    constexpr std::string_view kSuffix = ".format.json";
    std::vector<std::string> names;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string const leaf = it->path().filename().generic_string();
        if (leaf.size() <= kSuffix.size()) continue;
        if (leaf.compare(leaf.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        names.push_back(leaf.substr(0, leaf.size() - kSuffix.size()));
    }
    std::sort(names.begin(), names.end());
    for (auto const& name : names) {
        auto loaded = ObjectFormatSchema::loadShipped(name);
        if (!loaded.has_value()) {
            ADD_FAILURE() << name << " must load; a document this sweep cannot "
                             "read is a hole in it, never a pass";
            continue;
        }
        ++documents;
        out[std::string{objectFormatKindName((*loaded)->kind())}].push_back(*loaded);
    }
    return out;
}

// ── SIDE B: the descriptor corpus, read through ITS OWN LOADER ──────────────
//
// `readShippedLibDescriptor`, for the same reason side A uses the format
// loader. The read is done with NO resolver, so the WHOLE map is seen as
// DECLARED: every role entry lands in `libraryRoles` (every format key, not
// only the one a particular build selects — an arm no current target selects
// must not rot) and every literal lands in `library`. By construction a role
// entry yields no `library` string without a resolver, so the two maps
// partition the corpus exactly.
struct RoleReference {
    std::string        descriptor;   // config-root-relative, forward slashes
    std::string        context;      // "(root)" or "symbols['name']"
    std::string        format;       // the `library` map key = an object-format KIND name
    RuntimeLibraryRole role;
};

struct LiteralEntry {
    std::string descriptor;
    std::string context;
    std::string format;
    std::string image;
};

struct Corpus {
    std::vector<RoleReference> roles;
    std::vector<LiteralEntry>  literals;
    std::vector<std::string>   descriptors;   // relPaths, sorted — every one that read
};

[[nodiscard]] std::vector<fs::path> descriptorPaths(fs::path const& dir) {
    std::error_code       ec;
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it{dir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".json") continue;
        paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

// stdio.json's `vfprintf` spells the ABI alias `va_list`; without a binding the
// read fails loud. Any consistent stand-in works — nothing here reads a TypeId
// (the `test_shipped_type_consistency` precedent).
struct ReadContext {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    std::array<NamedTypeBinding, 1> named{
        NamedTypeBinding{"va_list",
                         interner.pointer(interner.primitive(TypeKind::Void))}};
};

[[nodiscard]] Corpus loadCorpus() {
    Corpus out;
    auto const root = configRoot();
    if (root.empty()) return out;
    fs::path const  dir = root / "shippedLibs";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        ADD_FAILURE() << "shippedLibs directory not found at " << dir;
        return out;
    }
    ReadContext ctx;
    for (auto const& path : descriptorPaths(dir)) {
        DiagnosticReporter rep;
        auto desc = readShippedLibDescriptor(path, ctx.interner, ctx.typeReg, rep,
                                             DataModel::Lp64,
                                             /*activeTarget=*/std::nullopt,
                                             /*activeFormat=*/std::nullopt,
                                             ctx.named,
                                             /*roleResolver=*/nullptr);
        // A descriptor that does not READ is a different invariant (pinned by
        // test_shipped_lib_descriptor) — but it MUST NOT silently shrink this
        // sweep, so it is surfaced rather than skipped in silence.
        EXPECT_TRUE(desc.has_value())
            << path.filename().generic_string() << " failed to read";
        if (!desc.has_value()) continue;
        std::string const rel = fs::relative(path, dir, ec).generic_string();
        out.descriptors.push_back(rel);
        for (auto const& [fmt, role] : desc->libraryRoles)
            out.roles.push_back(RoleReference{rel, "(root)", fmt, role});
        for (auto const& [fmt, img] : desc->library)
            out.literals.push_back(LiteralEntry{rel, "(root)", fmt, img});
        for (auto const& sym : desc->symbols) {
            for (auto const& [fmt, role] : sym.libraryRoles) {
                out.roles.push_back(RoleReference{
                    rel, "symbols['" + sym.name + "']", fmt, role});
            }
            for (auto const& [fmt, img] : sym.library) {
                out.literals.push_back(LiteralEntry{
                    rel, "symbols['" + sym.name + "']", fmt, img});
            }
        }
    }
    return out;
}

[[nodiscard]] LiteralException const* exceptionFor(std::string_view format,
                                                   std::string_view image) {
    for (auto const& e : kLiteralExceptions)
        if (e.format == format && e.image == image) return &e;
    return nullptr;
}

// The image a (kind, role) resolves to on EVERY flavour of the kind, through
// the driver's own adapter; failures are reported against `who`. Returns the
// agreed image, or nullopt after reporting.
[[nodiscard]] std::optional<std::string>
familyImageFor(Flavours const& flavours, std::string const& kind,
               RuntimeLibraryRole role, std::string const& who) {
    auto const group = flavours.find(kind);
    if (group == flavours.end()) {
        ADD_FAILURE() << who << ": no shipped object-format document declares kind '"
                      << kind << "', so the role reference has no family to resolve in";
        return std::nullopt;
    }
    std::optional<std::string> agreed;
    for (auto const& flavour : group->second) {
        FormatRuntimeLibraryRoleResolver const resolver{*flavour};
        std::string refusal;
        auto const* const row = resolver.rowForRole(role, refusal);
        EXPECT_TRUE(refusal.empty())
            << who << ": resolving role '" << runtimeLibraryRoleName(role)
            << "' on flavour '" << flavour->name() << "' was REFUSED: " << refusal;
        if (!refusal.empty()) return std::nullopt;
        EXPECT_NE(row, nullptr)
            << who << ": role '" << runtimeLibraryRoleName(role)
            << "' resolves to NOTHING on flavour '" << flavour->name()
            << "' — no document of kind '" << kind << "' declares it, so every "
               "symbol of this descriptor would be bound to no image on that build";
        if (row == nullptr) return std::nullopt;
        EXPECT_FALSE(row->image.empty())
            << who << ": role '" << runtimeLibraryRoleName(role)
            << "' is REALIZED from a shipped source on flavour '"
            << flavour->name() << "' (" << row->source
            << "); a descriptor imports from an IMAGE, so this reference cannot "
               "be honoured — declare the body under `realization` instead";
        if (row->image.empty()) return std::nullopt;
        if (!agreed.has_value()) {
            agreed = row->image;
        } else {
            EXPECT_EQ(*agreed, row->image)
                << who << ": role '" << runtimeLibraryRoleName(role)
                << "' resolves to '" << row->image << "' on flavour '"
                << flavour->name() << "' but to '" << *agreed
                << "' on an earlier flavour of the same kind — which flavour a "
                   "program happens to build would decide which runtime it gets";
            if (*agreed != row->image) return std::nullopt;
        }
    }
    return agreed;
}

}  // namespace

// ── (1) EVERY REFERENCED ROLE RESOLVES ON EVERY FLAVOUR OF ITS FAMILY ───────
TEST(DescriptorLibraryRoleAgreement,
     EveryRoleTheCorpusReferencesResolvesToOneImageOnEveryFlavourOfItsFamily) {
    std::size_t documents = 0;
    auto const  flavours  = loadFlavoursByKind(documents);
    ASSERT_GE(documents, 20u)
        << "the shipped object-format corpus must have been walked; only "
        << documents << " document(s) loaded";
    auto const corpus = loadCorpus();
    ASSERT_FALSE(corpus.roles.empty())
        << "no descriptor names any runtime role — the migration this file "
           "guards has been reverted, or the reader no longer records roles";

    std::set<std::pair<std::string, std::string>> pairs;
    std::set<std::string>                         kinds;
    std::set<std::string>                         rolesSeen;
    std::size_t                                   flavoursChecked = 0;
    for (auto const& r : corpus.roles) {
        pairs.emplace(r.format, std::string{runtimeLibraryRoleName(r.role)});
        kinds.insert(r.format);
        rolesSeen.insert(std::string{runtimeLibraryRoleName(r.role)});
    }
    for (auto const& [kind, roleName] : pairs) {
        auto const role = runtimeLibraryRoleFromName(roleName);
        ASSERT_TRUE(role.has_value()) << roleName;
        std::string who = "(" + kind + ", " + roleName + ")";
        auto const image = familyImageFor(flavours, kind, *role, who);
        EXPECT_TRUE(image.has_value()) << who;
        auto const group = flavours.find(kind);
        if (group != flavours.end()) flavoursChecked += group->second.size();
    }

    // ⚠ NON-VACUITY. Each count closes a different way this sweep could pass
    // having compared nothing: the corpus was read at all, the role references
    // are the measured population rather than a handful, all three families
    // with a C runtime are represented, more than one ROLE is referenced (a
    // guard exercising one role would pass while the other's rows rotted), and
    // every family was checked on more than one flavour — the family path
    // (a flavour with no own row) is what this file was written for.
    EXPECT_GE(corpus.descriptors.size(), 40u)
        << "only " << corpus.descriptors.size() << " descriptor(s) read";
    EXPECT_GE(corpus.roles.size(), 60u)
        << "only " << corpus.roles.size() << " role reference(s) collected — the "
           "migration covered 67";
    EXPECT_GE(kinds.size(), 3u) << "only " << kinds.size() << " kind(s) referenced";
    EXPECT_GE(rolesSeen.size(), 2u)
        << "only " << rolesSeen.size() << " distinct role(s) referenced; "
           "`cLibrary` and `systemPrimitives` are both bound by the corpus";
    EXPECT_GE(flavoursChecked, 2u * pairs.size())
        << "some family was checked on a single flavour; the whole point is the "
           "flavours that declare no row of their own";
}

// ── (2) THE RATCHET: NO REMAINING LITERAL RESTATES A ROLE'S IMAGE ───────────
TEST(DescriptorLibraryRoleAgreement, NoRemainingLiteralRestatesAnyRolesImage) {
    std::size_t documents = 0;
    auto const  flavours  = loadFlavoursByKind(documents);
    ASSERT_GE(documents, 20u);
    auto const corpus = loadCorpus();
    ASSERT_FALSE(corpus.literals.empty())
        << "no descriptor names a literal image any more — the stated exception "
           "(`libm.so.6`) has gone, and its disappearance must be argued, not "
           "absorbed";

    std::size_t compared = 0;
    for (auto const& lit : corpus.literals) {
        auto const group = flavours.find(lit.format);
        if (group == flavours.end()) continue;   // (1) reports an unknown kind
        for (auto const& flavour : group->second) {
            for (auto const& row : flavour->runtimeLibraries().bindings) {
                ++compared;
                EXPECT_NE(row.image, lit.image)
                    << lit.descriptor << " " << lit.context << ": `library."
                    << lit.format << "` spells '" << lit.image
                    << "' as a LITERAL, but flavour '" << flavour->name()
                    << "' declares that image as the '"
                    << runtimeLibraryRoleName(row.role)
                    << "' role. That is a second owner of one fact — the "
                       "duplication this row ended — and a repoint of the role "
                       "would leave this descriptor behind, silently, until "
                       "load. Write {\"role\": \""
                    << runtimeLibraryRoleName(row.role) << "\"} instead.";
            }
        }
    }
    EXPECT_GE(compared, 1u)
        << "no literal was compared against any role row — the ratchet held "
           "nothing";
}

// ── (3) THE ESCAPE, ENUMERATED BOTH WAYS AND ASSERTED IN THE OPPOSITE DIRECTION
TEST(DescriptorLibraryRoleAgreement, TheLiteralPopulationIsExactlyTheStatedExceptions) {
    std::size_t documents = 0;
    auto const  flavours  = loadFlavoursByKind(documents);
    ASSERT_GE(documents, 20u);
    auto const corpus = loadCorpus();

    std::set<std::pair<std::string, std::string>> used;
    for (auto const& lit : corpus.literals) {
        used.emplace(lit.format, lit.image);
        EXPECT_NE(exceptionFor(lit.format, lit.image), nullptr)
            << lit.descriptor << " " << lit.context << ": `library." << lit.format
            << "` names '" << lit.image
            << "' as a literal, which this file carries no exception for. A "
               "literal is allowed ONLY for an image that plays no runtime role "
               "on its family; every other image is named by its role. Either "
               "migrate the entry or argue the exception here.";
    }
    for (auto const& e : kLiteralExceptions) {
        EXPECT_NE(used.find({std::string{e.format}, std::string{e.image}}), used.end())
            << "the exception for (" << e.format << ", " << e.image
            << ") is never used: no shipped descriptor spells that literal any "
               "more. Delete the row — a classification kept past the "
               "configuration it classified is how an exception list stops "
               "refusing anything.";
    }

    // The opposite direction: an exception must play NO role — through the
    // driver's own adapter on every flavour, so the family path is what is
    // asked, and against every selectable role, not merely the one a reader
    // had in mind.
    std::size_t checked = 0;
    for (auto const& e : kLiteralExceptions) {
        auto const group = flavours.find(std::string{e.format});
        ASSERT_NE(group, flavours.end()) << e.format;
        for (auto const& flavour : group->second) {
            FormatRuntimeLibraryRoleResolver const resolver{*flavour};
            for (auto const roleName : kSelectableRuntimeLibraryRoleNames) {
                auto const role = runtimeLibraryRoleFromName(roleName);
                ASSERT_TRUE(role.has_value()) << roleName;
                std::string refusal;
                auto const* const row = resolver.rowForRole(*role, refusal);
                EXPECT_TRUE(refusal.empty()) << flavour->name() << ": " << refusal;
                ++checked;
                if (row == nullptr) continue;
                EXPECT_NE(row->image, std::string{e.image})
                    << "(" << e.format << ", " << e.image
                    << ") is classified as playing NO runtime role, but role '"
                    << roleName << "' resolves to exactly that image on flavour '"
                    << flavour->name()
                    << "'. The exception has become a duplicate. Reclassify it "
                       "with the role it now plays — " << e.why;
            }
        }
    }
    EXPECT_GE(checked, 4u)
        << "the exception was checked against " << checked
        << " (flavour, role) pair(s); the `libm.so.6` row must be tried against "
           "every selectable role on every elf flavour";
}

// ── (4) THE DECODE ITSELF, OVER THE REAL CORPUS ─────────────────────────────
//
// Reading each descriptor WITH the adapter over EACH shipped flavour of a kind
// it references must put, in the plain-string `library` map every consumer
// reads, exactly the image (1) established for that family. This is the
// production chokepoint (`decodeLibraryMap`) run over the shipped corpus for
// every flavour, own-row and family path alike; a resolver that answered but a
// decode that dropped the answer would pass (1) and ship an unbound import.
TEST(DescriptorLibraryRoleAgreement, EveryRoleEntryDecodesToItsFamilysImageThroughTheReader) {
    std::size_t documents = 0;
    auto const  flavours  = loadFlavoursByKind(documents);
    ASSERT_GE(documents, 20u);
    auto const corpus = loadCorpus();
    ASSERT_FALSE(corpus.roles.empty());

    // Which descriptors reference which kinds (root or symbol), and the
    // family's agreed image per (kind, role) from the adapter.
    std::map<std::string, std::set<std::string>> kindsByDescriptor;
    std::map<std::pair<std::string, std::string>, std::string> familyImage;
    for (auto const& r : corpus.roles) {
        kindsByDescriptor[r.descriptor].insert(r.format);
        auto const key = std::make_pair(r.format,
                                        std::string{runtimeLibraryRoleName(r.role)});
        if (familyImage.count(key) != 0) continue;
        auto const image = familyImageFor(flavours, r.format, r.role, r.descriptor);
        if (!image.has_value()) return;   // (1) has reported it
        familyImage.emplace(key, *image);
    }

    auto const root = configRoot();
    ASSERT_FALSE(root.empty());
    fs::path const dir = root / "shippedLibs";
    ReadContext    ctx;
    std::size_t    decoded = 0;
    std::size_t    flavourReads = 0;
    for (auto const& path : descriptorPaths(dir)) {
        std::error_code   ec;
        std::string const rel = fs::relative(path, dir, ec).generic_string();
        auto const kinds = kindsByDescriptor.find(rel);
        if (kinds == kindsByDescriptor.end()) continue;   // names no role
        for (auto const& kind : kinds->second) {
            auto const group = flavours.find(kind);
            ASSERT_NE(group, flavours.end()) << kind;
            for (auto const& flavour : group->second) {
                FormatRuntimeLibraryRoleResolver const resolver{*flavour};
                DiagnosticReporter rep;
                auto desc = readShippedLibDescriptor(
                    path, ctx.interner, ctx.typeReg, rep, DataModel::Lp64,
                    /*activeTarget=*/std::nullopt, /*activeFormat=*/std::nullopt,
                    ctx.named, &resolver);
                ASSERT_TRUE(desc.has_value())
                    << rel << " failed to read with the resolver over '"
                    << flavour->name() << "'";
                ++flavourReads;
                auto const expectFor =
                    [&](std::unordered_map<std::string, RuntimeLibraryRole> const& roles,
                        std::unordered_map<std::string, std::string> const&  library,
                        std::string const& context) {
                        auto const role = roles.find(kind);
                        if (role == roles.end()) return;
                        auto const want = familyImage.find(
                            {kind, std::string{runtimeLibraryRoleName(role->second)}});
                        ASSERT_NE(want, familyImage.end());
                        auto const got = library.find(kind);
                        EXPECT_NE(got, library.end())
                            << rel << " " << context << ": `library." << kind
                            << "` names role '"
                            << runtimeLibraryRoleName(role->second)
                            << "' but the reader put NO image in the map when "
                               "resolving over '" << flavour->name()
                            << "' — every symbol of this descriptor would reach "
                               "the link unbound";
                        if (got == library.end()) return;
                        EXPECT_EQ(got->second, want->second)
                            << rel << " " << context << ": `library." << kind
                            << "` decoded to '" << got->second << "' over '"
                            << flavour->name() << "' but the family's '"
                            << runtimeLibraryRoleName(role->second)
                            << "' image is '" << want->second << "'";
                        ++decoded;
                    };
                expectFor(desc->libraryRoles, desc->library, "(root)");
                for (auto const& sym : desc->symbols)
                    expectFor(sym.libraryRoles, sym.library,
                              "symbols['" + sym.name + "']");
            }
        }
    }
    EXPECT_GE(decoded, 60u)
        << "only " << decoded << " role entr(ies) were decoded and compared";
    EXPECT_GE(flavourReads, 100u)
        << "only " << flavourReads
        << " (descriptor, flavour) read(s) — every flavour of each referenced "
           "family must have been tried";
}
