#include "link/object_format_schema.hpp"

#include "core/substrate/relocation_table.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace dss {

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadFromFile(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             path.string(), "cannot open file"}});
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return loadFromText(std::move(buf).str(), path.string());
}

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadShipped(std::string_view name) {
    auto path = findShippedConfig({name, "object-formats", ".format.json",
                                   "object format",
                                   DiagnosticCode::C_InvalidLanguageName});
    if (!path) return std::unexpected(std::move(path).error());
    return loadFromFile(*path);
}

namespace detail {

namespace {

ConfigDiagnostic makeProblem(std::string path, std::string message) {
    return ConfigDiagnostic{
        DiagnosticCode::C_MalformedJson,
        DiagnosticSeverity::Error,
        std::move(path),
        std::move(message),
    };
}

} // namespace

std::vector<ConfigDiagnostic> ObjectFormatData::validate() const {
    std::vector<ConfigDiagnostic> problems;
    auto fail = [&](std::string path, std::string msg) {
        problems.push_back(makeProblem(std::move(path), std::move(msg)));
    };

    // Format kind must be a real shipped format. `Unknown` is the
    // invalid sentinel — a JSON file that names "unknown" (or omits
    // the field, which the loader rejects upstream) is not a real
    // format declaration.
    if (kind == ObjectFormatKind::Unknown) {
        fail("/format/kind",
             "format kind 'unknown' is reserved as the invalid sentinel; "
             "declare one of 'elf' / 'pe' / 'macho' / 'wasm' / 'spirv'");
    }

    // Cross-row reloc uniqueness + non-empty-name + non-zero-kind:
    // shared substrate with TargetSchema so the two sides of plan
    // 13 §2.6's reloc-taxonomy unifier are validated identically.
    substrate::validateRelocationsTable<ObjectFormatRelocationInfo>(
        relocations, fail);

    // nativeId is the format's actual wire value (e.g. ELF R_X86_64_PC32
    // = 2 in `r_info` low 32 bits). Zero would silently write a
    // R_X86_64_NONE relocation that the linker treats as a no-op — a
    // miscompile that round-trips as syntactically valid. Reject at
    // load time when relocations[] is non-empty.
    for (std::size_t i = 0; i < relocations.size(); ++i) {
        if (relocations[i].nativeId == 0) {
            fail(std::format("/relocations/{}/nativeId", i),
                 std::format("relocation '{}': 'nativeId' must be != 0 "
                             "(the format-specific wire tag, e.g. ELF "
                             "R_X86_64_PC32 = 2)",
                             relocations[i].name));
        }
    }

    // Sections: kind unique cross-row + name non-empty. The format
    // walker resolves `sectionByKind(SectionKind::Text)` to find
    // the format-native section name + structural fields.
    {
        std::unordered_map<SectionKind, std::size_t> seenSection;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            auto const& s = sections[i];
            if (s.name.empty()) {
                fail(std::format("/sections/{}/name", i),
                     "section row: 'name' must be a non-empty string");
            }
            auto [it, fresh] = seenSection.emplace(s.kind, i);
            if (!fresh) {
                fail(std::format("/sections/{}/kind", i),
                     std::format("section '{}': duplicate 'kind' value "
                                 "(already declared by section '{}' at "
                                 "/sections/{})",
                                 s.name, sections[it->second].name,
                                 it->second));
            }
        }
    }

    // ELF identity: when format kind is Elf, the identity block must
    // be populated. `fileClass=0` means "no class declared" which
    // would emit an invalid ELF header byte.
    if (kind == ObjectFormatKind::Elf) {
        if (elf.fileClass == 0) {
            fail("/elf/class", "ELF format requires 'elf.class' "
                               "(one of 'elf32' / 'elf64')");
        }
        if (elf.dataEncoding == 0) {
            fail("/elf/data", "ELF format requires 'elf.data' "
                              "(one of 'lsb' / 'msb')");
        }
        if (elf.machine == 0) {
            fail("/elf/machine", "ELF format requires 'elf.machine' "
                                 "(EM_* value, e.g. 62 for x86_64, "
                                 "183 for aarch64)");
        }
        // e_type — the LK1 cycle 2 ELF walker supports ET_REL (1)
        // and ET_EXEC (2). ET_DYN (3) is anchored at D-LK1-4 for
        // PIE/shared-lib support paired with LK6 dynamic linking.
        // Reject unknown values at load time so a typo in a JSON
        // can't reach the walker.
        if (elf.objectType != 1 && elf.objectType != 2) {
            fail("/elf/type",
                 std::format("ELF format 'elf.type' = {} not supported; "
                             "cycle 2 ships 'rel' (=ET_REL=1) and 'exec' "
                             "(=ET_EXEC=2). ET_DYN=3 (PIE / .so) is "
                             "anchored at plan 14 §3.1 D-LK1-4 paired "
                             "with LK6 dynamic linking.",
                             elf.objectType));
        }
        // ET_EXEC schemas must declare which sections are loaded and
        // at what virtual address. Today the walker uses sh_addr =
        // section.virtualAddress directly (no relocation of
        // virtualAddress). When `virtualAddress == 0` for SectionKind::
        // Text on an ET_EXEC schema, the walker would emit an
        // executable loaded at virtual address 0 — null-deref on
        // first instruction. Reject explicitly.
        if (elf.objectType == 2) {
            auto const* secText = [&]() -> ObjectFormatSectionInfo const* {
                for (auto const& s : sections) {
                    if (s.kind == SectionKind::Text) return &s;
                }
                return nullptr;
            }();
            if (secText != nullptr && secText->virtualAddress == 0) {
                fail("/sections/<text>/virtualAddress",
                     "ELF ET_EXEC format requires `virtualAddress != 0` "
                     "on the .text section row — the walker uses this "
                     "as sh_addr and as the base for e_entry. Loading "
                     ".text at virtual address 0 would null-deref on "
                     "the first instruction. Typical Linux x86_64 base "
                     "is 0x400000 (per linker convention).");
            }
        }
        // Conversely, ET_REL must NOT carry virtual addresses (they're
        // set by the LINKER at exec build time, not declared on the
        // .o's section rows). A non-zero `virtualAddress` here would
        // be silently dropped when emitting `sh_addr = 0` for the
        // .o. Reject so a JSON edit can't no-op.
        if (elf.objectType == 1) {
            for (std::size_t i = 0; i < sections.size(); ++i) {
                if (sections[i].virtualAddress != 0) {
                    fail(std::format("/sections/{}/virtualAddress", i),
                         std::format("section '{}': 'virtualAddress' "
                                     "must be 0 for ELF ET_REL format "
                                     "rows (sh_addr in relocatable .o "
                                     "is unbound; the linker assigns "
                                     "addresses at exec build time)",
                                     sections[i].name));
                }
            }
        }
    }

    // PE/COFF identity: when format kind is Pe, machine must be
    // declared. `Characteristics=0` is a legitimate value for
    // relocatable .obj (the linker sets image-level flags), so we
    // don't reject it here.
    if (kind == ObjectFormatKind::Pe) {
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
            // PE derives runtime virtual addresses from ImageBase +
            // RVA in its IMAGE_OPTIONAL_HEADER (cycle-2 PE32+ path).
            // The substrate `virtualAddress` field is meaningless
            // for PE rows; reject explicitly so a future PE-row
            // edit can't silently no-op (LK1-cycle-2 invariant).
            if (sections[i].virtualAddress != 0) {
                fail(std::format("/sections/{}/virtualAddress", i),
                     std::format("section '{}': 'virtualAddress' must "
                                 "be 0 for PE format rows (PE derives "
                                 "section VAs from "
                                 "IMAGE_OPTIONAL_HEADER.ImageBase + "
                                 "section RVA at link time, not from "
                                 "the substrate field)",
                                 sections[i].name));
            }
        }
    }

    // Mach-O identity: cputype/cpusubtype/filetype must be declared.
    // Mach-O is also the only format whose section rows REQUIRE a
    // non-empty `segment` (two-level naming) — ELF/PE leave it
    // empty. Section + segment names also must fit in 16 chars
    // (Mach-O has no long-name escape; the walker writes a fixed
    // 16-byte field).
    if (kind == ObjectFormatKind::MachO) {
        if (macho.cputype == 0) {
            fail("/macho/cputype",
                 "Mach-O format requires 'macho.cputype' (CPU_TYPE_* "
                 "value, e.g. 0x01000007 for x86_64, 0x0100000C for "
                 "arm64)");
        }
        if (macho.filetype == 0) {
            fail("/macho/filetype",
                 "Mach-O format requires 'macho.filetype' (MH_OBJECT=1 "
                 "for relocatable .o; cycle 1 ships MH_OBJECT only)");
        }
        for (std::size_t i = 0; i < sections.size(); ++i) {
            auto const& s = sections[i];
            if (s.segment.empty()) {
                fail(std::format("/sections/{}/segment", i),
                     std::format("section '{}': Mach-O requires a "
                                 "non-empty 'segment' name (e.g. "
                                 "'__TEXT' for the section '__text'); "
                                 "the single-string substrate field is "
                                 "for ELF/PE only",
                                 s.name));
            }
            if (s.name.size() > 16) {
                fail(std::format("/sections/{}/name", i),
                     std::format("section '{}' length {} exceeds the "
                                 "16-char Mach-O section_64.sectname "
                                 "field (no long-name escape exists)",
                                 s.name, s.name.size()));
            }
            if (s.segment.size() > 16) {
                fail(std::format("/sections/{}/segment", i),
                     std::format("segment '{}' length {} exceeds the "
                                 "16-char Mach-O section_64.segname "
                                 "field",
                                 s.segment, s.segment.size()));
            }
            // Mach-O MH_OBJECT files use section_64.addr = 0 (vmaddr
            // assignment happens at exec build time via LC_SEGMENT_64).
            // MH_EXECUTE will use virtualAddress; that arm lands at
            // D-LK3-2. For cycle 1 (filetype == MH_OBJECT), reject
            // non-zero virtualAddress to prevent silent drift.
            if (macho.filetype == 1 && s.virtualAddress != 0) {
                fail(std::format("/sections/{}/virtualAddress", i),
                     std::format("section '{}': 'virtualAddress' must "
                                 "be 0 for Mach-O MH_OBJECT rows "
                                 "(section_64.addr in relocatable .o "
                                 "is 0; exec-image VAs land at "
                                 "D-LK3-2)",
                                 s.name));
            }
        }
        // Mach-O nativeId packing reserves bit 27 (r_extern) and
        // bits 0..23 (r_symbolnum) for walker-filled fields. JSON
        // values setting those bits silently corrupt the
        // relocation_info word at emit time (silent-failure C1 +
        // type-design Q4 convergence). Validate the packing mask
        // here so JSON typos fail loud at load time.
        constexpr std::uint32_t kMachOReservedBits =
            (1u << 27) | 0x00FFFFFFu;
        for (std::size_t i = 0; i < relocations.size(); ++i) {
            auto const& r = relocations[i];
            if ((r.nativeId & kMachOReservedBits) != 0) {
                fail(std::format("/relocations/{}/nativeId", i),
                     std::format("relocation '{}': 'nativeId' "
                                 "0x{:08X} sets reserved bits "
                                 "(bit 27 = r_extern is walker-filled; "
                                 "bits 0..23 = r_symbolnum are filled "
                                 "per-reloc with the symtab index). "
                                 "Mach-O packing must only set bits "
                                 "28..31 (r_type), 25..26 (r_length), "
                                 "and 24 (r_pcrel)",
                                 r.name, r.nativeId));
            }
        }
    }

    // ELF + PE sections must NOT carry a segment name (the field is
    // Mach-O-specific). Reject explicitly so a JSON edit can't
    // silently no-op.
    if (kind == ObjectFormatKind::Elf || kind == ObjectFormatKind::Pe) {
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (!sections[i].segment.empty()) {
                fail(std::format("/sections/{}/segment", i),
                     std::format("section '{}': 'segment' must be empty "
                                 "for ELF/PE rows (only Mach-O uses the "
                                 "two-level (segment, section) naming)",
                                 sections[i].name));
            }
        }
    }

    return problems;
}

} // namespace detail

} // namespace dss
