#pragma once

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/format/byte_emit.hpp"

#include <format>
#include <span>
#include <string_view>

namespace dss::link::format {

// ★ A CARRIED HANDLER THE `.eh_frame` WRITERS CANNOT STATE YET IS REFUSED, NEVER
// DROPPED (P69 fold 3c).
//
// `AssembledFunction::languageHandler` is a foreign object's handler routine and
// its data. The PE writer states it in the function's UNWIND_INFO. The ELF and
// Mach-O writers build `.eh_frame` from `cfi` alone: their CIE names no
// personality and their FDE points at no language-specific data area, so a
// function carrying one would be described WITHOUT its handler — an exception
// dispatched to that frame would find no routine, and the image would look
// complete. Until those writers state the CIE's personality and the FDE's data
// area, each refuses such a function by name, before any byte.
//
// Returns true when it refused (the diagnostic is reported).
[[nodiscard]] inline bool refuseAHandlerTheEhFrameCannotState(std::span<AssembledFunction const> functions,
                                                              std::string_view                   writer,
                                                              DiagnosticReporter&                reporter) {
    for (std::size_t fi = 0; fi < functions.size(); ++fi) {
        if (!functions[fi].languageHandler.has_value()) continue;
        detail::emit(reporter, DiagnosticCode::K_UnwindRuleUnrepresentable,
             std::format("{}: function #{} carries the handler routine and data its object named for it, and "
                         "this writer's `.eh_frame` states neither a personality nor a language-specific data "
                         "area yet; describing the function without its handler would leave an exception "
                         "dispatched to its frame with no routine to call.",
                         writer, fi));
        return true;
    }
    return false;
}

} // namespace dss::link::format
