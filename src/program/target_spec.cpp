#include "program/target_spec.hpp"

#include <cctype>
#include <string>

namespace dss {

namespace {

[[nodiscard]] bool containsWhitespace(std::string_view s) noexcept {
    for (unsigned char c : s) {
        if (std::isspace(c)) return true;
    }
    return false;
}

} // namespace

std::string_view targetSpecErrorName(TargetSpecError e) noexcept {
    switch (e) {
        case TargetSpecError::MissingColon:     return "MissingColon";
        case TargetSpecError::MultipleColons:   return "MultipleColons";
        case TargetSpecError::EmptyTargetName:  return "EmptyTargetName";
        case TargetSpecError::EmptyFormatName:  return "EmptyFormatName";
        case TargetSpecError::WhitespaceInName: return "WhitespaceInName";
    }
    return "Unknown";
}

std::expected<TargetSpec, TargetSpecError>
TargetSpec::parse(std::string_view spec) noexcept {
    auto const colon = spec.find(':');
    if (colon == std::string_view::npos) {
        return std::unexpected(TargetSpecError::MissingColon);
    }
    if (spec.find(':', colon + 1) != std::string_view::npos) {
        return std::unexpected(TargetSpecError::MultipleColons);
    }
    auto const tgt = spec.substr(0, colon);
    auto const fmt = spec.substr(colon + 1);
    if (tgt.empty()) return std::unexpected(TargetSpecError::EmptyTargetName);
    if (fmt.empty()) return std::unexpected(TargetSpecError::EmptyFormatName);
    if (containsWhitespace(tgt) || containsWhitespace(fmt)) {
        return std::unexpected(TargetSpecError::WhitespaceInName);
    }
    return TargetSpec{std::string{tgt}, std::string{fmt}};
}

std::string_view TargetSpec::outputExtension(
        ObjectFormatSchema const& fmt) const noexcept {
    switch (fmt.kind()) {
        case ObjectFormatKind::Elf:
            switch (fmt.elf().objectType) {
                case ElfObjectType::Rel:  return ".o";
                case ElfObjectType::Exec: return "";
                case ElfObjectType::Dyn:
                    // c151 (D-LK1-4 PIE half): ET_DYN is a `.so` OR
                    // a PIE executable — the schema's entry cluster
                    // discriminates (validate() pins it all-or-none;
                    // `processExit` is the canonical single-member
                    // witness). A PIE takes executable naming: no
                    // extension, exactly like ET_EXEC (`gcc -pie
                    // hello.c -o prog` names it `prog`, not
                    // `prog.so`).
                    return fmt.processExit().has_value() ? "" : ".so";
            }
            return "";
        case ObjectFormatKind::Pe:
            switch (fmt.pe().objectType) {
                case PeObjectType::Obj:   return ".obj";
                case PeObjectType::Exec:  return ".exe";
                case PeObjectType::Dll:   return ".dll";
            }
            return "";
        case ObjectFormatKind::MachO:
            switch (fmt.macho().filetype) {
                case MachOObjectType::Object:  return ".o";
                case MachOObjectType::Execute: return "";
                case MachOObjectType::Dylib:   return ".dylib";
            }
            return "";
        case ObjectFormatKind::Wasm:    return ".wasm";
        case ObjectFormatKind::Spirv:   return ".spv";
        case ObjectFormatKind::Unknown: return "";
    }
    return "";
}

} // namespace dss
