// ─────────────────────────────────────────────────────────────────────────
// PE/COFF object-format backend.
// D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125).
// ─────────────────────────────────────────────────────────────────────────
//
// This TU is the SANCTIONED REALIZATION TIER. It is allowed to know that it
// is PE/COFF — that is the whole reason the seam exists. What it must never
// do is let the shared substrate know: `object_format_schema.cpp` and
// `object_format_schema_json.cpp` reach this code only through the abstract
// `ObjectFormatBackend`, and they cannot even SPELL `ObjectFormatKind::Pe`
// (both TUs carry a compile-error pin that makes the name ambiguous).
//
// ★ EVERY RULE BODY BELOW WAS MOVED VERBATIM. The `validateIdentity` and
// `readIdentity` bodies are byte-for-byte the blocks that used to sit behind
// `if (kind == ObjectFormatKind::Pe)` in the schema triple — same rules,
// same wording, same JSON pointers. The pointers are load-bearing: 163
// `countAtPath` assertions across `tests/link/` pin diagnostics at exact
// pointers, and this refactor is only correct if every one of them still
// resolves. The rules moved; their pointers did not.
//
// The binding preamble at the top of `validateIdentity` re-establishes the
// unqualified member names the moved code used when it was a member of
// `ObjectFormatData::validate()`. Renaming those references instead would
// have made a 1,000-line move impossible to verify by inspection — and a rule
// that silently starts reading a different field is exactly the class of
// defect this refactor must not introduce.

#include "link/format/object_format_backends.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/types/config_key_vocabulary.hpp"  // detail::renderAllowedList
#include "core/types/parse_diagnostic.hpp"
#include "link/format/pe.hpp"
#include "link/object_format_schema.hpp"

#include "link/object_format_identity_doc.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss::link::format {
namespace {

char const* const kPeBlocks[] = { "pe", "optionalHeader" };

// D-SQLITE-PE64-FULL-TIER-STACK-DEPTH. The ONE stack-reserve vehicle any
// walker implements today: IMAGE_OPTIONAL_HEADER64.SizeOfStackReserve, at
// optional-header offset +72, written by this backend's image arm.
//
// ★ THIS IS THE CAPABILITY DECLARATION THAT REPLACED `kVehicleKinds[]` — the
// table keyed on `ObjectFormatKind` that the loader used to consult. The
// coherence rule it fed still runs, unchanged in what it rejects: a schema
// naming a vehicle this backend does not claim still fails loud at load. What
// changed is that the loader now asks the OWNER whether it implements the
// vehicle, instead of consulting a table that named the owner by identity.
//
// ⚠ AND THE CAPABILITY IS NOT UNIFORM WITHIN THIS BACKEND'S OWN FORMATS.
// `pe64-x86_64-windows-exec` declares `stackReserveControl`;
// `pe64-x86_64-windows-dll` does not, because the Windows loader ignores a
// DLL's SizeOfStackReserve. Both resolve to THIS backend. That is precisely
// why the schema-side question is "did this FORMAT declare the block?" and
// never "is this backend PE?" — the second question gets the .dll wrong.
constexpr StackReserveVehicle kPeVehicles[] = {
    StackReserveVehicle::PeOptionalHeader,
};

class PeBackend final : public ObjectFormatBackend {
public:
    [[nodiscard]] std::string_view configName() const noexcept override {
        return "pe";
    }
    [[nodiscard]] ObjectFormatKind kind() const noexcept override {
        return ObjectFormatKind::Pe;
    }
    [[nodiscard]] std::span<char const* const>
    identityBlockNames() const noexcept override { return kPeBlocks; }
    [[nodiscard]] std::span<char const* const>
    rejectedRootFields() const noexcept override { return {}; }
    [[nodiscard]] std::string_view
    rejectedRootFieldsReason() const noexcept override { return {}; }
    [[nodiscard]] std::span<StackReserveVehicle const>
    stackReserveVehicles() const noexcept override { return kPeVehicles; }

    [[nodiscard]] bool
    isImageFlavor(detail::ObjectFormatData const& d) const noexcept override {
        return d.pe.objectType != PeObjectType::Obj;
    }
    [[nodiscard]] bool
    isExecFlavor(detail::ObjectFormatData const& d) const noexcept override {
        return d.pe.objectType == PeObjectType::Exec;
    }

    // c152 (D-LK2-4): PE Dll is FALSE. Windows has no ld.so-style deferred
    // global scope for implicitly-linked DLLs — every import binds at load
    // from a NAMED module's export table, so a referenced no-library extern
    // still has nothing to resolve it and must reject loud at build time.
    [[nodiscard]] bool allowsUndefinedImports(
            detail::ObjectFormatData const&) const noexcept override {
        return false;
    }

    [[nodiscard]] bool isRelocatableMember(
            detail::ObjectFormatData const& d) const noexcept override {
        return d.pe.objectType == PeObjectType::Obj;
    }
    [[nodiscard]] bool sectionsCarrySegmentNames() const noexcept override {
        return false;
    }

    void readIdentity(ObjectFormatIdentityDoc const&  docWrap,
                      detail::ObjectFormatData&       data,
                      substrate::DiagnosticCollector& coll) const override {
        // Unwrap once; the relocated reader bodies below are verbatim and
        // spell `doc` exactly as they did in the loader.
        nlohmann::json const& doc = docWrap.json();

        // PE/COFF identity block — read only when format kind is Pe.
        if (doc.contains("pe")) {
            auto const& p = doc.at("pe");
            if (!p.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/pe",
                          "'pe' must be an object when format.kind == 'pe'");
            } else {
                auto readU16 = [&](char const* field, std::uint16_t& out,
                                   std::int64_t max) {
                    if (!p.contains(field) || !p.at(field).is_number_integer())
                        return;
                    std::int64_t const v = p.at(field).get<std::int64_t>();
                    if (v < 0 || v > max) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/pe/{}", field),
                                  std::format("'{}' ({}) out of range [0, {}]",
                                              field, v, max));
                        return;
                    }
                    out = static_cast<std::uint16_t>(v);
                };
                readU16("machine", data.pe.machine, 0xFFFF);
                readU16("characteristics", data.pe.characteristics, 0xFFFF);
                // `type`: closed-enum PeObjectType (obj/exec/dll).
                // Default Obj keeps LK2 cycle 1 schemas unchanged.
                if (p.contains("type") && p.at("type").is_string()) {
                    auto const tName = p.at("type").get<std::string>();
                    auto const tEnum = peObjectTypeFromName(tName);
                    if (tEnum.has_value()) {
                        data.pe.objectType = *tEnum;
                    } else {
                        // D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-
                        // CLOSED-SET: projected from the table the lookup
                        // above consults, never retyped beside it.
                        coll.emit(DiagnosticCode::C_MalformedJson, "/pe/type",
                                  std::format("'type' must be {}",
                                              ::dss::detail::renderAllowedList(
                                                  allNames(kPeObjectTypeTable),
                                                  " / ")));
                    }
                }
            }
        }

        // PE32+ Optional Header — read only when PE objectType != Obj.
        // The walker emits the optional header for Exec/Dll; Obj schemas
        // never carry it, and validate() rejects an `optionalHeader` key
        // on an Obj schema as a load-time config error (symmetric with
        // ELF ET_REL's virtualAddress=0 rejection).
        if (doc.contains("optionalHeader")) {
            auto const& oh = doc.at("optionalHeader");
            if (!oh.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/optionalHeader",
                          "'optionalHeader' must be an object");
            } else {
                auto readU16 = [&](char const* field, std::uint16_t& out) {
                    if (!oh.contains(field)) return;
                    if (!oh.at(field).is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/optionalHeader/{}", field),
                                  std::format("'{}' must be an integer", field));
                        return;
                    }
                    std::int64_t const v = oh.at(field).get<std::int64_t>();
                    if (v < 0 || v > 0xFFFF) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/optionalHeader/{}", field),
                                  std::format("'{}' ({}) out of u16 range",
                                              field, v));
                        return;
                    }
                    out = static_cast<std::uint16_t>(v);
                };
                auto readU32 = [&](char const* field, std::uint32_t& out) {
                    if (!oh.contains(field)) return;
                    if (!oh.at(field).is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/optionalHeader/{}", field),
                                  std::format("'{}' must be an integer", field));
                        return;
                    }
                    std::int64_t const v = oh.at(field).get<std::int64_t>();
                    if (v < 0 || v > 0xFFFFFFFFLL) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/optionalHeader/{}", field),
                                  std::format("'{}' ({}) out of u32 range",
                                              field, v));
                        return;
                    }
                    out = static_cast<std::uint32_t>(v);
                };
                auto readU64 = [&](char const* field, std::uint64_t& out) {
                    if (!oh.contains(field)) return;
                    if (!oh.at(field).is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/optionalHeader/{}", field),
                                  std::format("'{}' must be an integer", field));
                        return;
                    }
                    std::int64_t const v = oh.at(field).get<std::int64_t>();
                    if (v < 0) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/optionalHeader/{}", field),
                                  std::format("'{}' ({}) must be non-negative",
                                              field, v));
                        return;
                    }
                    out = static_cast<std::uint64_t>(v);
                };
                readU16("magic", data.peOptionalHeader.magic);
                readU64("imageBase", data.peOptionalHeader.imageBase);
                readU32("sectionAlignment",
                        data.peOptionalHeader.sectionAlignment);
                readU32("fileAlignment", data.peOptionalHeader.fileAlignment);
                readU16("majorOperatingSystemVersion",
                        data.peOptionalHeader.majorOperatingSystemVersion);
                readU16("minorOperatingSystemVersion",
                        data.peOptionalHeader.minorOperatingSystemVersion);
                readU16("majorSubsystemVersion",
                        data.peOptionalHeader.majorSubsystemVersion);
                readU16("minorSubsystemVersion",
                        data.peOptionalHeader.minorSubsystemVersion);
                readU16("subsystem", data.peOptionalHeader.subsystem);
                readU16("dllCharacteristics",
                        data.peOptionalHeader.dllCharacteristics);
                readU64("sizeOfStackReserve",
                        data.peOptionalHeader.sizeOfStackReserve);
                readU64("sizeOfStackCommit",
                        data.peOptionalHeader.sizeOfStackCommit);
                readU64("sizeOfHeapReserve",
                        data.peOptionalHeader.sizeOfHeapReserve);
                readU64("sizeOfHeapCommit",
                        data.peOptionalHeader.sizeOfHeapCommit);
                // Plan 14 LK7 — Authenticode codesign placeholder
                // reservation. Optional; defaults to 0 (no reservation,
                // no security directory entry). Multiple-of-8 enforced
                // at validate() (PE COFF §5.9.1 alignment).
                readU32("attributeCertReserveSize",
                        data.peOptionalHeader.attributeCertReserveSize);
            }
        }
    }

    void validateIdentity(detail::ObjectFormatData const& d,
                          SchemaProblemSink const&        fail) const override {
        // Binding preamble — see the file header.
        auto const& sections               = d.sections;
        auto const& pe                     = d.pe;
        auto const& peOptionalHeader       = d.peOptionalHeader;
        auto const& entryPoint             = d.entryPoint;
        auto const& processExit            = d.processExit;
        auto const& entryCallingConvention = d.entryCallingConvention;
        auto const& processArgs            = d.processArgs;

    // PE/COFF identity: when format kind is Pe, machine must be
    // declared. `Characteristics=0` is a legitimate value for
    // relocatable .obj (the linker sets image-level flags), so we
    // don't reject it here.
        {
        if (pe.machine == 0) {
            fail("/pe/machine", "PE format requires 'pe.machine' "
                                "(IMAGE_FILE_MACHINE_* value, e.g. "
                                "0x8664 for x86_64, 0xAA64 for arm64)");
        }
        // PE encodes section alignment in Characteristics bits
        // IMAGE_SCN_ALIGN_*BYTES (which live in the substrate `type`
        // field), so neither `addrAlign` NOR `flags` is meaningful
        // for PE rows. Reject both explicitly to prevent the silent-
        // mismatch hazard a future maintainer would hit when they
        // edit a PE JSON's addrAlign/flags expecting them to take
        // effect (type-design Q3 + architect Decision 2/4 conv.).
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (sections[i].addrAlign != 0) {
                fail(std::format("/sections/{}/addrAlign", i),
                     std::format("section '{}': 'addrAlign' must be 0 "
                                 "for PE format rows (PE encodes "
                                 "alignment in Characteristics bits "
                                 "IMAGE_SCN_ALIGN_*BYTES via the "
                                 "substrate 'type' field; setting "
                                 "addrAlign here would be silently "
                                 "ignored)",
                                 sections[i].name));
            }
            if (sections[i].flags != 0) {
                fail(std::format("/sections/{}/flags", i),
                     std::format("section '{}': 'flags' must be 0 for "
                                 "PE format rows (PE folds ALL section "
                                 "flags into Characteristics via the "
                                 "substrate 'type' field; the 'flags' "
                                 "field is meaningful only for ELF "
                                 "sh_flags)",
                                 sections[i].name));
            }
            // PE virtualAddress semantics depend on objectType:
            //   * Obj (.obj relocatable): must be 0 — the linker
            //     binds section VAs at exec build time.
            //   * Exec/Dll (PE32+ image): non-zero declares the
            //     section's RVA (Relative Virtual Address — the
            //     OFFSET from ImageBase, NOT the absolute VA). PE
            //     stores `virtualAddress` field as the RVA in
            //     IMAGE_SECTION_HEADER; the kernel maps
            //     `ImageBase + RVA` at load time.
            if (pe.objectType == PeObjectType::Obj
             && sections[i].virtualAddress != 0) {
                fail(std::format("/sections/{}/virtualAddress", i),
                     std::format("section '{}': 'virtualAddress' must "
                                 "be 0 for PE .obj (relocatable) format "
                                 "rows. .obj does NOT carry RVAs — the "
                                 "linker binds VAs at exec build time. "
                                 "For PE32+ executable images, set "
                                 "pe.type = 'exec' and declare RVAs "
                                 "explicitly.",
                                 sections[i].name));
            }
            if (pe.objectType != PeObjectType::Obj
             && sections[i].kind == SectionKind::Text
             && sections[i].virtualAddress == 0) {
                fail(std::format("/sections/{}/virtualAddress", i),
                     std::format("section '{}': PE32+ {} image requires "
                                 "non-zero 'virtualAddress' (the RVA of "
                                 ".text — typical 0x1000 for a minimal "
                                 ".exe, immediately after the headers in "
                                 "the first 4 KB page).",
                                 sections[i].name,
                                 std::string{peObjectTypeName(pe.objectType)}));
            }
            // PE/COFF §3.4: section RVAs must be multiples of
            // SectionAlignment. The Windows loader silently rejects
            // images whose section RVAs straddle alignment
            // boundaries — surface at validate() time. (silent-
            // failure C3 + code-reviewer C3 convergence)
            if (pe.objectType != PeObjectType::Obj
             && peOptionalHeader.sectionAlignment != 0
             && sections[i].virtualAddress != 0
             && sections[i].virtualAddress
                    % peOptionalHeader.sectionAlignment != 0) {
                fail(std::format("/sections/{}/virtualAddress", i),
                     std::format("section '{}': PE32+ image section "
                                 "'virtualAddress' (0x{:x}) must be a "
                                 "multiple of 'sectionAlignment' "
                                 "(0x{:x}) per PE/COFF §3.4.",
                                 sections[i].name,
                                 sections[i].virtualAddress,
                                 peOptionalHeader.sectionAlignment));
            }
            // PE/COFF: RVAs are 32-bit (the entire IMAGE_OPTIONAL_
            // HEADER64 storage for SizeOfImage / SizeOfHeaders /
            // section RVAs is u32). A schema declaring a >4 GiB
            // virtualAddress would silently narrow at emit time
            // (silent-failure H2 post-audit fold).
            if (pe.objectType != PeObjectType::Obj
             && sections[i].virtualAddress
                    > std::numeric_limits<std::uint32_t>::max()) {
                fail(std::format("/sections/{}/virtualAddress", i),
                     std::format("section '{}': PE32+ section "
                                 "'virtualAddress' (0x{:x}) exceeds "
                                 "u32 — PE/COFF RVAs are 32-bit.",
                                 sections[i].name,
                                 sections[i].virtualAddress));
            }
        }
        }

    // PE32+ optional header rules. Mirrors the ELF ET_REL/ET_EXEC
    // symmetry on virtualAddress: a PE .obj must NOT declare an
    // optional header (validate-reject any non-zero magic — the
    // walker writes 0 for SizeOfOptionalHeader on .obj); a PE
    // Exec/Dll image MUST declare every load-bearing field (Magic,
    // ImageBase, alignments, subsystem, stack/heap sizes).
        {
        auto const& oh = peOptionalHeader;
        bool const isObj = pe.objectType == PeObjectType::Obj;
        if (isObj) {
            bool const anySet = oh.magic != 0 || oh.imageBase != 0
                || oh.sectionAlignment != 0 || oh.fileAlignment != 0
                || oh.subsystem != 0 || oh.sizeOfStackReserve != 0
                || oh.sizeOfStackCommit != 0 || oh.sizeOfHeapReserve != 0
                || oh.sizeOfHeapCommit != 0 || oh.dllCharacteristics != 0
                || oh.attributeCertReserveSize != 0;
            if (anySet) {
                fail("/optionalHeader",
                     "PE .obj (relocatable) format must NOT declare an "
                     "'optionalHeader' — the optional header (incl. "
                     "attributeCertReserveSize) lives only in PE32+ "
                     "executable images (.exe / .dll). Set pe.type = "
                     "'exec' if this schema describes an executable.");
            }
        } else {
            // Exec/Dll: every load-bearing field must be set.
            if (oh.magic != 0x10B && oh.magic != 0x20B) {
                fail("/optionalHeader/magic",
                     "PE32+ optional header 'magic' must be 0x20B "
                     "(PE32+) or 0x10B (PE32). v1 ships PE32+ on "
                     "x86_64-windows.");
            }
            // PE32+ images (`.exe` AND `.dll`) MUST set
            // `IMAGE_FILE_EXECUTABLE_IMAGE` (0x0002) in
            // `IMAGE_FILE_HEADER.Characteristics` — the bit means
            // "this image is fully linked and loadable", NOT "this is
            // a .exe" (PE COFF §3.3.2: "If this flag is not set, it
            // indicates a linker error"); without it the Windows
            // loader silently refuses the file with
            // `ERROR_BAD_EXE_FORMAT` and no diagnostic. dumpbin
            // ground truth (c152): a `cl /LD` DLL carries 0x2022 =
            // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL
            // — both bits together, retiring the earlier
            // "mutually exclusive" misreading this guard's Exec-only
            // scope was based on. (architect post-fold review,
            // LK7-readiness gap for LK10 hermetic e2e; widened to the
            // Dll arm at c152 / D-LK2-4.)
            constexpr std::uint16_t IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002;
            constexpr std::uint16_t IMAGE_FILE_DLL              = 0x2000;
            if ((pe.characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0u) {
                fail("/pe/characteristics",
                     std::format("PE32+ image ({}) requires the "
                                 "IMAGE_FILE_EXECUTABLE_IMAGE bit "
                                 "(0x0002) set in 'pe.characteristics' "
                                 "(got 0x{:04x}) -- the bit means "
                                 "'fully linked, loadable image' and a "
                                 "DLL carries it too (cl /LD emits "
                                 "0x2022); without it the Windows "
                                 "loader fails ERROR_BAD_EXE_FORMAT "
                                 "with no user-visible diagnostic.",
                                 std::string{peObjectTypeName(pe.objectType)},
                                 pe.characteristics));
            }
            // The IMAGE_FILE_DLL bit (0x2000) is the .dll/.exe
            // discriminator the LOADER reads: CreateProcess rejects an
            // image carrying it; LoadLibrary requires it to treat the
            // module as a library. It must agree with the schema's
            // declared objectType both ways — a mismatch would emit an
            // artifact whose extension (`TargetSpec::outputExtension`)
            // and loader behavior contradict each other. c152, D-LK2-4.
            if (pe.objectType == PeObjectType::Dll
             && (pe.characteristics & IMAGE_FILE_DLL) == 0u) {
                fail("/pe/characteristics",
                     std::format("PE32+ dynamic-link library (pe.type "
                                 "= 'dll') requires the IMAGE_FILE_DLL "
                                 "bit (0x2000) set in "
                                 "'pe.characteristics' (got 0x{:04x}) "
                                 "-- without it the emitted .dll is a "
                                 "mis-labeled executable image. cl /LD "
                                 "ground truth: 0x2022. D-LK2-4.",
                                 pe.characteristics));
            }
            if (pe.objectType == PeObjectType::Exec
             && (pe.characteristics & IMAGE_FILE_DLL) != 0u) {
                fail("/pe/characteristics",
                     std::format("PE32+ executable image (pe.type = "
                                 "'exec') must NOT set the "
                                 "IMAGE_FILE_DLL bit (0x2000) in "
                                 "'pe.characteristics' (got 0x{:04x}) "
                                 "-- CreateProcess rejects an image "
                                 "carrying it (a copy-paste from the "
                                 "dll sibling). D-LK2-4.",
                                 pe.characteristics));
            }
            if (oh.imageBase == 0) {
                fail("/optionalHeader/imageBase",
                     "PE32+ image requires non-zero 'imageBase' "
                     "(preferred load address; typical 0x140000000 for "
                     ".exe, 0x180000000 for .dll).");
            }
            // sectionAlignment / fileAlignment: power-of-two, and
            // sectionAlignment >= fileAlignment (PE/COFF §3.4).
            auto const isPow2 = [](std::uint64_t v) noexcept {
                return v > 0 && (v & (v - 1)) == 0;
            };
            if (!isPow2(oh.sectionAlignment)) {
                fail("/optionalHeader/sectionAlignment",
                     "PE32+ 'sectionAlignment' must be a positive "
                     "power-of-two (typical 4096 = 0x1000 — page size).");
            }
            // PE/COFF §3.4: sectionAlignment >= page size (4096 on
            // x86_64). The Windows loader rejects sub-page section
            // alignment with STATUS_INVALID_IMAGE_FORMAT. ARM64-
            // Windows uses 4 KB pages too; this constant is uniform
            // for current Windows targets (silent-failure C3 + code-
            // reviewer C3 convergence).
            if (oh.sectionAlignment != 0 && oh.sectionAlignment < 4096u) {
                fail("/optionalHeader/sectionAlignment",
                     std::format("PE32+ 'sectionAlignment' ({}) must "
                                 "be >= 4096 (page size). Windows "
                                 "loader rejects sub-page alignment "
                                 "with STATUS_INVALID_IMAGE_FORMAT.",
                                 oh.sectionAlignment));
            }
            if (!isPow2(oh.fileAlignment)) {
                fail("/optionalHeader/fileAlignment",
                     "PE32+ 'fileAlignment' must be a positive "
                     "power-of-two in [512, 65536] per PE/COFF §3.4 "
                     "(typical 512 = 0x200).");
            }
            if (oh.fileAlignment != 0
             && (oh.fileAlignment < 512 || oh.fileAlignment > 65536)) {
                fail("/optionalHeader/fileAlignment",
                     std::format("PE32+ 'fileAlignment' ({}) must be in "
                                 "[512, 65536] per PE/COFF §3.4.",
                                 oh.fileAlignment));
            }
            if (oh.sectionAlignment != 0 && oh.fileAlignment != 0
             && oh.sectionAlignment < oh.fileAlignment) {
                fail("/optionalHeader/sectionAlignment",
                     std::format("PE32+ requires sectionAlignment ({}) "
                                 ">= fileAlignment ({}) per spec §3.4.",
                                 oh.sectionAlignment, oh.fileAlignment));
            }
            if (oh.subsystem == 0) {
                fail("/optionalHeader/subsystem",
                     "PE32+ image requires non-zero 'subsystem' "
                     "(IMAGE_SUBSYSTEM_WINDOWS_CUI=3 / WINDOWS_GUI=2).");
            }
            if (oh.sizeOfStackReserve == 0 || oh.sizeOfStackCommit == 0
             || oh.sizeOfHeapReserve == 0 || oh.sizeOfHeapCommit == 0) {
                fail("/optionalHeader",
                     "PE32+ image requires non-zero "
                     "sizeOfStackReserve / sizeOfStackCommit / "
                     "sizeOfHeapReserve / sizeOfHeapCommit (typical "
                     "0x100000 reserve / 0x1000 commit).");
            }
            // Plan 14 LK7 — attribute-cert reservation must be a
            // multiple of 8 (PE COFF §5.9.1 — `WIN_CERTIFICATE.
            // dwLength` is 8-byte-aligned; the table itself sits
            // at an 8-byte-aligned file offset so its entries can
            // be parsed as packed u32 fields). 0 = no reservation
            // (default); any other value must align so plan 16's
            // attribute-cert blob fills without padding mid-table.
            if (oh.attributeCertReserveSize != 0
             && (oh.attributeCertReserveSize % 8u) != 0u) {
                fail("/optionalHeader/attributeCertReserveSize",
                     std::format("'optionalHeader."
                                 "attributeCertReserveSize' ({}) must "
                                 "be a multiple of 8 (PE COFF §5.9.1 "
                                 "attribute-cert table alignment; "
                                 "plan 16 fills the reserved bytes "
                                 "with WIN_CERTIFICATE entries).",
                                 oh.attributeCertReserveSize));
            }
        }
        // ── PE Dll shape rules — c152, D-LK2-4 (the PE mirror of the
        // ELF ET_DYN `.so` shape block above). A DSS `.dll` ships with
        // AddressOfEntryPoint = 0 — NO DllMain (legal PE: the loader
        // skips process/thread notifications for an entry-less
        // module; `link /NOENTRY` produces the same shape) — so the
        // entry cluster is ILLEGAL here, exactly as on the `.so`. A
        // future DllMain-bearing DLL is the pinned follow-up
        // D-LK2-DLL-DLLMAIN-ENTRY: it would legalize the cluster on
        // the dll shape via a discriminator (the c151 PIE pattern),
        // never by relaxing this reject silently.
        if (pe.objectType == PeObjectType::Dll) {
            bool sawText = false;
            for (auto const& s : sections) {
                if (s.kind == SectionKind::Text) sawText = true;
            }
            if (!sawText) {
                fail("/sections",
                     "PE32+ dll format requires a Text section row "
                     "(SectionKind::Text) -- a dynamic-link library "
                     "without code has nothing to export. D-LK2-4.");
            }
            if (processExit.has_value() || !entryCallingConvention.empty()
             || processArgs.has_value()) {
                fail("/pe",
                     std::format(
                         "PE32+ dll format declares entry-cluster "
                         "machinery -- processExit: {}, "
                         "entryCallingConvention: {}, processArgs: {}. "
                         "A DSS .dll has NO DllMain (AddressOfEntryPoint "
                         "= 0; the loader skips the notification call), "
                         "so no entry trampoline is synthesized and the "
                         "cluster is dead config that would silently "
                         "diverge from the emitted image. The "
                         "DllMain-bearing arm is the pinned follow-up "
                         "D-LK2-DLL-DLLMAIN-ENTRY. D-LK2-4.",
                         processExit.has_value() ? "present" : "absent",
                         entryCallingConvention.empty() ? "absent"
                                                        : "present",
                         processArgs.has_value() ? "present" : "absent"));
            }
            if (!entryPoint.empty()) {
                fail("/entryPoint",
                     std::format("PE32+ dll format must not declare "
                                 "'entryPoint' (got '{}') -- a DSS .dll "
                                 "has no process entry "
                                 "(AddressOfEntryPoint = 0, no DllMain; "
                                 "D-LK2-DLL-DLLMAIN-ENTRY is the pinned "
                                 "follow-up). D-LK2-4.",
                                 entryPoint));
            }
        }
        }
    }

    [[nodiscard]] std::vector<std::uint8_t>
    encode(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& objectFormatSchema,
           DiagnosticReporter&       reporter,
           ImageRequest const&       request) const override {
        // The one walker that consumes `request` — it implements the
        // `pe-optional-header` vehicle declared above, for the exec flavor
        // only. `pe::encode` RE-CHECKS the schema's declared capability
        // itself rather than trusting the linker gate, because it is also a
        // public entry point and it serves the .dll flavor too.
        return pe::encode(module, targetSchema, objectFormatSchema, reporter,
                          request);
    }
};

} // namespace

// ★ FUNCTION-LOCAL STATIC, NOT A NAMESPACE-SCOPE ONE, and the difference is
// load-bearing. `objectFormatBackendTable()` takes this object's ADDRESS from
// another TU. A namespace-scope static would make that a static-INITIALIZATION-
// ORDER dependency across translation units: the address is valid before
// construction, but the VTABLE POINTER is not, so a table built during static
// init would hold an object whose virtual calls are undefined. The magic-static
// below is constructed on first call, in order, always. (Unreachable today —
// nothing calls the table during static init — which is exactly why it would
// have been found the hard way.)
ObjectFormatBackend const& peBackend() noexcept {
    static PeBackend const instance{};
    return instance;
}

} // namespace dss::link::format
