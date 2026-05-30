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
        // e_type — the LK1 cycle 2 ELF walker supports ET_REL and
        // ET_EXEC. ET_DYN is declared on the closed enum but
        // rejected here until D-LK1-4 closes (PIE/.so paired with
        // LK6 dynamic linking).
        if (elf.objectType != ElfObjectType::Rel
         && elf.objectType != ElfObjectType::Exec) {
            fail("/elf/type",
                 std::format("ELF format 'elf.type' = '{}' not yet "
                             "supported by the walker; cycle 2 ships "
                             "'rel' and 'exec'. 'dyn' (ET_DYN: PIE / "
                             ".so) is anchored at plan 14 §3.1 D-LK1-4 "
                             "paired with LK6 dynamic linking.",
                             std::string{elfObjectTypeName(elf.objectType)}));
        }
        // ET_EXEC schemas must declare which sections are loaded and
        // at what virtual address. Today the walker uses sh_addr =
        // section.virtualAddress directly (no relocation of
        // virtualAddress). When `virtualAddress == 0` for SectionKind::
        // Text on an ET_EXEC schema, the walker would emit an
        // executable loaded at virtual address 0 — null-deref on
        // first instruction. Reject explicitly.
        if (elf.objectType == ElfObjectType::Exec) {
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
            // PT_LOAD `p_align` — the Linux kernel rejects ELF
            // executables whose `p_align` is smaller than the
            // runtime page size (`ENOEXEC` at exec time, silent
            // from the toolchain's POV). x86_64 Linux uses 4 KB;
            // ARM64 Linux on Apple Silicon / certain Graviton
            // kernels uses 16 KB or 64 KB. Each (arch × OS) ELF
            // exec schema declares its own value — D-LK6-3.
            if (elf.pageAlign == 0) {
                fail("/elf/pageAlign",
                     "ELF ET_EXEC format must declare 'elf.pageAlign' "
                     "(PT_LOAD p_align). The kernel rejects exec'd "
                     "images whose p_align is smaller than the "
                     "runtime page size. Common values: 4096 "
                     "(x86_64 Linux, ARM64 4K pages), 16384 (Apple "
                     "Silicon Asahi 16K), 65536 (ARM64 64K pages on "
                     "some Graviton / embedded kernels). Declaration "
                     "is mandatory per (arch × OS) — anchored at plan "
                     "14 §3.1 D-LK6-3.");
            }
        }
        // Conversely, ET_REL must NOT carry virtual addresses (they're
        // set by the LINKER at exec build time, not declared on the
        // .o's section rows). A non-zero `virtualAddress` here would
        // be silently dropped when emitting `sh_addr = 0` for the
        // .o. Reject so a JSON edit can't no-op.
        if (elf.objectType == ElfObjectType::Rel) {
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
        }
    }

    // PE32+ optional header rules. Mirrors the ELF ET_REL/ET_EXEC
    // symmetry on virtualAddress: a PE .obj must NOT declare an
    // optional header (validate-reject any non-zero magic — the
    // walker writes 0 for SizeOfOptionalHeader on .obj); a PE
    // Exec/Dll image MUST declare every load-bearing field (Magic,
    // ImageBase, alignments, subsystem, stack/heap sizes).
    if (kind == ObjectFormatKind::Pe) {
        auto const& oh = peOptionalHeader;
        bool const isObj = pe.objectType == PeObjectType::Obj;
        if (isObj) {
            bool const anySet = oh.magic != 0 || oh.imageBase != 0
                || oh.sectionAlignment != 0 || oh.fileAlignment != 0
                || oh.subsystem != 0 || oh.sizeOfStackReserve != 0
                || oh.sizeOfStackCommit != 0 || oh.sizeOfHeapReserve != 0
                || oh.sizeOfHeapCommit != 0 || oh.dllCharacteristics != 0;
            if (anySet) {
                fail("/optionalHeader",
                     "PE .obj (relocatable) format must NOT declare an "
                     "'optionalHeader' — the optional header lives only "
                     "in PE32+ executable images (.exe / .dll). Set "
                     "pe.type = 'exec' if this schema describes an "
                     "executable.");
            }
        } else {
            // Exec/Dll: every load-bearing field must be set.
            if (oh.magic != 0x10B && oh.magic != 0x20B) {
                fail("/optionalHeader/magic",
                     "PE32+ optional header 'magic' must be 0x20B "
                     "(PE32+) or 0x10B (PE32). v1 ships PE32+ on "
                     "x86_64-windows.");
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
        // LK3 cycle 1 + cycle 2 walker arms support MH_OBJECT (1)
        // and MH_EXECUTE (2). MH_DYLIB (6) is declared on the closed
        // enum but rejected at validate() until D-LK3-3 closes
        // (paired with LK6 dynamic linking, same shape as ELF
        // ET_DYN's D-LK1-4 anchor).
        if (macho.filetype != MachOObjectType::Object
         && macho.filetype != MachOObjectType::Execute) {
            fail("/macho/filetype",
                 std::format("Mach-O 'macho.filetype' = '{}' not yet "
                             "supported by the walker; cycles 1+2 ship "
                             "MH_OBJECT and MH_EXECUTE. MH_DYLIB is "
                             "anchored at plan 14 §3.1 D-LK3-3 paired "
                             "with LK6 dynamic linking.",
                             std::string{machoObjectTypeName(macho.filetype)}));
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
            if (macho.filetype == MachOObjectType::Object
             && s.virtualAddress != 0) {
                fail(std::format("/sections/{}/virtualAddress", i),
                     std::format("section '{}': 'virtualAddress' must "
                                 "be 0 for Mach-O MH_OBJECT rows "
                                 "(section_64.addr in relocatable .o "
                                 "is 0; MH_EXECUTE rows declare VAs "
                                 "explicitly — LK3 cycle 2)",
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
        // MH_EXECUTE / MH_DYLIB image rules. The walker emits
        // LC_LOAD_DYLINKER (the dynamic linker path) and
        // LC_LOAD_DYLIB (each library the image needs at load
        // time). __PAGEZERO segment vmsize comes from
        // machoImage.pageZeroSize. All three are mandatory for
        // any loadable Mach-O image — the loader rejects the
        // binary at exec time if they're missing or zero. Reject
        // at load time instead.
        auto const& mi = machoImage;
        bool const isObj = macho.filetype == MachOObjectType::Object;
        if (isObj) {
            bool const anySet = mi.pageZeroSize != 0
                || !mi.dylinkerPath.empty()
                || !mi.loadDylibs.empty();
            if (anySet) {
                fail("/image",
                     "Mach-O MH_OBJECT format must NOT declare an "
                     "'image' block — pageZeroSize / dylinkerPath / "
                     "loadDylibs live only in MH_EXECUTE / MH_DYLIB "
                     "images. Set macho.filetype = 2 (MH_EXECUTE) if "
                     "this schema describes an executable.");
            }
        } else if (macho.filetype == MachOObjectType::Execute) {
            if (mi.pageZeroSize == 0) {
                fail("/image/pageZeroSize",
                     "Mach-O MH_EXECUTE image requires non-zero "
                     "'image.pageZeroSize' (__PAGEZERO segment vmsize "
                     "— typical 0x100000000 = 4 GiB on x86_64/ARM64 "
                     "darwin; catches null-pointer derefs at the "
                     "kernel level).");
            }
            if (mi.dylinkerPath.empty()) {
                fail("/image/dylinkerPath",
                     "Mach-O MH_EXECUTE image requires non-empty "
                     "'image.dylinkerPath' (LC_LOAD_DYLINKER — "
                     "typical '/usr/lib/dyld' on macOS).");
            }
            if (mi.loadDylibs.empty()) {
                fail("/image/loadDylibs",
                     "Mach-O MH_EXECUTE image requires at least one "
                     "'image.loadDylibs' entry (typical "
                     "'/usr/lib/libSystem.B.dylib' — libc / process "
                     "start function provider).");
            }
            for (std::size_t i = 0; i < mi.loadDylibs.size(); ++i) {
                if (mi.loadDylibs[i].path.empty()) {
                    fail(std::format("/image/loadDylibs/{}/path", i),
                         "loadDylibs entries must declare a non-empty "
                         "path");
                }
            }
            // __TEXT segment must sit at or above __PAGEZERO's end —
            // otherwise the walker's `sectionVa - pageZeroSize`
            // subtraction underflows silently to ~2^64 and the
            // emitted segment vmsize wraps (silent-failure H4 + code-
            // reviewer C2 convergence).
            for (auto const& s : sections) {
                if (s.kind == SectionKind::Text
                 && s.virtualAddress < mi.pageZeroSize) {
                    fail("/sections/<text>/virtualAddress",
                         std::format("Mach-O MH_EXECUTE: __text "
                                     "virtualAddress 0x{:x} is below "
                                     "__PAGEZERO end 0x{:x} (pageZeroSize) "
                                     "— __TEXT would overlap __PAGEZERO; "
                                     "loader rejects.",
                                     s.virtualAddress, mi.pageZeroSize));
                }
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

    // Cross-format exec-flavor invariant (type-design Q5 convergence
    // + type-design O1 post-audit fold). The predicate mirrors
    // `ObjectFormatSchema::isImageFlavor()` exactly — the schema
    // and its private validate() helper share one disjunction (no
    // drift surface).
    //
    // A single source of truth tying the image-side triplet:
    //   format declares "executable mode" ⟺ a Text-section row
    //   declares a non-zero virtualAddress (where to load it).
    // `entryPoint` is independent (empty defaults to functions[0];
    // non-empty resolves by name) — NOT cross-tied here. All three
    // image arms (ELF ET_EXEC, PE PE32+ Exec/Dll, Mach-O MH_EXECUTE)
    // inherit this gate uniformly.
    bool const isExecFlavor =
        (kind == ObjectFormatKind::Elf
         && elf.objectType == ElfObjectType::Exec)
     || (kind == ObjectFormatKind::Pe
         && pe.objectType != PeObjectType::Obj)
     || (kind == ObjectFormatKind::MachO
         && macho.filetype == MachOObjectType::Execute);
    if (isExecFlavor) {
        // Walker requires Text + virtualAddress != 0 to compute
        // e_entry / p_vaddr / IMAGE_OPTIONAL_HEADER.ImageBase. The
        // per-format rule above already rejects ELF ET_EXEC with
        // text.virtualAddress == 0; this terminal pass restates the
        // contract uniformly so PE/MachO image arms inherit the
        // gate the same way (one rule covers all 3 formats).
        bool sawText = false;
        for (auto const& s : sections) {
            if (s.kind != SectionKind::Text) continue;
            sawText = true;
            if (s.virtualAddress == 0) {
                fail("/sections/<text>/virtualAddress",
                     "image-flavor format (ELF ET_EXEC / PE PE32+ / "
                     "Mach-O MH_EXECUTE) requires the Text section "
                     "row's `virtualAddress != 0`. The walker computes "
                     "the entry-point VA from this field; a value of "
                     "0 would emit an image loaded at virtual address "
                     "0, which the runtime kernel rejects as ENOEXEC.");
            }
        }
        if (!sawText) {
            fail("/sections",
                 "image-flavor format requires a Text section row "
                 "(SectionKind::Text). No such row was declared.");
        }
    }

    return problems;
}

} // namespace detail

} // namespace dss
