#include "program/target_spec.hpp"

namespace dss {

std::string_view outputExtensionFor(ObjectFormatSchema const& fmt) noexcept {
    // D-FF1-AR-STATICLIB-DRIVER-WIRING (c171): a static-library format
    // (`container: archive`) outputs an `ar` archive; its extension is the
    // ecosystem's static-lib convention — Unix `.a` (ELF / Mach-O) vs the
    // Microsoft `.lib` (COFF / PE) — NOT the member object's `.o`/`.obj`.
    // Keyed on `kind()` (the closed enum, the existing agnostic dispatch
    // axis), never a format-name branch.
    if (fmt.isStaticArchive()) {
        return fmt.kind() == ObjectFormatKind::Pe ? ".lib" : ".a";
    }
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
