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
        // field), so the `addrAlign` field is meaningless for PE
        // rows. Reject explicitly to prevent the silent-mismatch
        // hazard a future maintainer would hit when they edit a
        // PE JSON's addrAlign expecting it to take effect (type-
        // design Q3 + architect Decision 4 convergence).
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
        }
    }

    return problems;
}

} // namespace detail

} // namespace dss
