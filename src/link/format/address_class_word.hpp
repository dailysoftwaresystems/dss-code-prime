#pragma once

// ══ A WRITER WRITES ONE CLASS (P69 fold 3e, the review of fold 3d's X9, R9-R11) ═══════════════════════════════════
//
// Each object and image writer writes ONE class: ELF64 (elf.cpp), Mach-O 64 (macho.cpp), PE32+ (pe.cpp). Its address
// -- a GOT or import slot, a rebase site, a data pointer -- is that class's word, named ONCE in the writer and read by
// every slot product there. A document whose data model's pointer is another width cannot be written by it: every
// slot would be the class's width, not the program's. So the writer's one public entry (`encode`) asks this first and
// refuses such a document BY NAME -- the width, the writer, the format -- before anything is sized; no 32-bit class
// is ever written by accident, and a 32-bit class is a writer of its own when one ships.
//
// Pinned by tests/link/test_writer_address_class.cpp (every writer, each of its documents with a 4-byte data model:
// refused; the shipped documents: written).

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dss::link::format {

// True when `format`'s data model's pointer is `classWordBytes` wide; otherwise reports
// K_PointerWidthNotTheWritersClass, naming `writerEntry`, the format and both widths, and answers false.
[[nodiscard]] inline bool requireTheWritersAddressWidth(ObjectFormatSchema const& format,
                                                        std::uint64_t             classWordBytes,
                                                        std::string_view          writerEntry,
                                                        DiagnosticReporter&       reporter) {
    std::optional<std::uint64_t> const pointerBytes = scalarByteSize(TypeKind::Ptr, format.dataModel());
    if (pointerBytes.has_value() && *pointerBytes == classWordBytes) return true;
    ParseDiagnostic d;
    d.code     = DiagnosticCode::K_PointerWidthNotTheWritersClass;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::format(
        "{}: format '{}' states the data model '{}', whose pointer is {}; this writer writes ONE class, whose "
        "address -- every slot, rebase site and data pointer it writes -- is {} bytes, so the document is refused "
        "rather than written with slots of the wrong width.",
        writerEntry, format.name(), dataModelName(format.dataModel()),
        pointerBytes.has_value() ? std::to_string(*pointerBytes) + " bytes" : std::string{"of no stated width"},
        classWordBytes);
    reporter.report(std::move(d));
    return false;
}

}  // namespace dss::link::format
