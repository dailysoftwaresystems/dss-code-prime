#include "program/cross_validate_language_target.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <string>

namespace dss {

bool languageTargetIsaCompatible(GrammarSchema const& language,
                                 TargetSchema const&  target) noexcept {
    std::string_view const declared = language.isa();
    // PORTABILITY IS THE DEFAULT, AND IT IS THE FIRST BRANCH ON PURPOSE: the
    // overwhelming majority of compiles take exactly this line and touch
    // nothing else this file owns.
    if (declared.empty()) return true;
    // Fail-CLOSED on an undeclared target: `target.isa()` empty means the
    // document declares no architecture, and an unknown value can never be
    // SHOWN equal to a declared binding. Note this arm is reached only for a
    // language that HAS a binding — a portable language already returned true
    // above, so an ISA-less target keeps building every portable language
    // exactly as before.
    return declared == target.isa();
}

bool crossValidateLanguageTarget(GrammarSchema const& language,
                                 std::string_view     languageName,
                                 TargetSchema const&  target,
                                 std::string_view     targetSpec,
                                 std::string_view     subject,
                                 DiagnosticReporter&  reporter) {
    if (languageTargetIsaCompatible(language, target)) return true;

    // The target's half, rendered so the two arms of the failure read
    // differently. "declares no instruction-set architecture" and "declares
    // 'aarch64'" are different problems with different fixes, and a message
    // that printed an empty string for the first would look like a bug in the
    // compiler rather than a fact about the target document.
    std::string targetSide;
    if (target.isa().empty()) {
        targetSide = "target '" + std::string{target.name()}
                   + "' declares NO instruction-set architecture (no "
                     "'target.isa' key in its .target.json)";
    } else {
        targetSide = "target '" + std::string{target.name()}
                   + "' executes '" + std::string{target.isa()} + "'";
    }

    std::string msg;
    if (!subject.empty()) {
        msg += std::string{subject} + ": ";
    }
    msg += "language '" + std::string{languageName} + "' emits '"
         + std::string{language.isa()} + "' machine instructions, but "
         + targetSide + " — so this language CANNOT target that "
           "architecture. (Target spec: '" + std::string{targetSpec} + "'.)"
         + " This is not a missing entry in a list of supported platforms: "
           "the language's surface IS an instruction set, so nothing "
           "downstream can translate it. Build this source for a target whose "
           "'target.isa' is '" + std::string{language.isa()}
         + "', or write the same code in the dialect for the target's "
           "architecture. A language that declares no 'isa' at all is "
           "PORTABLE and builds for every target.";

    ParseDiagnostic d;
    d.code     = DiagnosticCode::D_LanguageTargetIsaMismatch;
    d.severity = DiagnosticSeverity::Error;
    // Guaranteed, NOT unsuppressable — the two are different questions and
    // this code answers them differently (see its allocation note). It is the
    // ONLY statement of why this pairing stopped, so the reporter's cap must
    // not be free to drop it on a diagnostic-heavy multi-target build; but
    // nothing distinguishes it from `D_ArtifactProfileFormatMismatch` on the
    // SUPPRESSION axis, so it does not join the closed table to obtain that.
    d.delivery = DiagnosticDelivery::Guaranteed;
    d.actual   = std::move(msg);
    reporter.report(std::move(d));
    return false;
}

} // namespace dss
