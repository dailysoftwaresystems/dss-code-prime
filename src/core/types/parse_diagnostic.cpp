#include "core/types/parse_diagnostic.hpp"

#include <format>

namespace dss {

std::string_view severityName(DiagnosticSeverity s) noexcept {
    switch (s) {
        case DiagnosticSeverity::Hint:    return "hint";
        case DiagnosticSeverity::Info:    return "info";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Error:   return "error";
    }
    return "error";  // unreachable; satisfy compilers without exhaustiveness inference
}

std::string_view diagnosticCodeName(DiagnosticCode c) noexcept {
    switch (c) {
        case DiagnosticCode::None:                       return "None";
        case DiagnosticCode::P_UnexpectedToken:          return "P_UnexpectedToken";
        case DiagnosticCode::P_MissingRequiredChild:     return "P_MissingRequiredChild";
        case DiagnosticCode::P_UnknownToken:             return "P_UnknownToken";
        case DiagnosticCode::P_PrematureEndOfInput:      return "P_PrematureEndOfInput";
        case DiagnosticCode::P_InvalidEscapeSequence:    return "P_InvalidEscapeSequence";
        case DiagnosticCode::P_NumericLiteralOutOfRange: return "P_NumericLiteralOutOfRange";
        case DiagnosticCode::P_DeprecatedSyntax:         return "P_DeprecatedSyntax";
        case DiagnosticCode::P_AmbiguousToken:           return "P_AmbiguousToken";
        case DiagnosticCode::P_NoAlternativeMatched:     return "P_NoAlternativeMatched";
        case DiagnosticCode::P_UnclosedScope:            return "P_UnclosedScope";
        case DiagnosticCode::P_UnmatchedClose:           return "P_UnmatchedClose";
        case DiagnosticCode::P_ContextualKeywordResolution:
            return "P_ContextualKeywordResolution";
        case DiagnosticCode::P_SchemaCursorDesync:
            return "P_SchemaCursorDesync";
        case DiagnosticCode::P_IllegalChar:              return "P_IllegalChar";
        case DiagnosticCode::P_MalformedNumber:          return "P_MalformedNumber";
        case DiagnosticCode::P_UnterminatedString:       return "P_UnterminatedString";
        case DiagnosticCode::P_UnterminatedComment:      return "P_UnterminatedComment";
        case DiagnosticCode::P_InvalidEscape:            return "P_InvalidEscape";
        case DiagnosticCode::P_BuilderInvariant:         return "P_BuilderInvariant";
        case DiagnosticCode::P_TooManyDiagnostics:       return "P_TooManyDiagnostics";
        case DiagnosticCode::P_UnfinishedTree:           return "P_UnfinishedTree";
        case DiagnosticCode::P_RecoveryStalled:          return "P_RecoveryStalled";
        case DiagnosticCode::P_MaxSpeculationDepth:      return "P_MaxSpeculationDepth";
        case DiagnosticCode::P_UncommittedCheckpoint:    return "P_UncommittedCheckpoint";
        case DiagnosticCode::P_BacktrackFailed:          return "P_BacktrackFailed";
        case DiagnosticCode::C_MissingField:             return "C_MissingField";
        case DiagnosticCode::C_UnknownShape:             return "C_UnknownShape";
        case DiagnosticCode::C_UnknownToken:             return "C_UnknownToken";
        case DiagnosticCode::C_VersionMismatch:          return "C_VersionMismatch";
        case DiagnosticCode::C_CircularShape:            return "C_CircularShape";
        case DiagnosticCode::C_AmbiguousAlternatives:    return "C_AmbiguousAlternatives";
        case DiagnosticCode::C_UnclosableScope:          return "C_UnclosableScope";
        case DiagnosticCode::C_MalformedJson:            return "C_MalformedJson";
        case DiagnosticCode::C_InvalidLanguageName:      return "C_InvalidLanguageName";
        case DiagnosticCode::C_InvalidPrecedenceTable:   return "C_InvalidPrecedenceTable";
        case DiagnosticCode::C_RedundantScopeRequire:    return "C_RedundantScopeRequire";
        case DiagnosticCode::C_ConflictingField:         return "C_ConflictingField";
        case DiagnosticCode::C_UnknownScopeName:         return "C_UnknownScopeName";
        case DiagnosticCode::C_RedundantField:           return "C_RedundantField";
        case DiagnosticCode::C_UnknownLexerMode:         return "C_UnknownLexerMode";
        case DiagnosticCode::C_InvalidStringStyle:       return "C_InvalidStringStyle";
        case DiagnosticCode::C_BodyDefaultKindInShape:   return "C_BodyDefaultKindInShape";
        case DiagnosticCode::D_FileNotFound:             return "D_FileNotFound";
        case DiagnosticCode::D_EmptyInput:               return "D_EmptyInput";
        case DiagnosticCode::D_DuplicateFile:            return "D_DuplicateFile";
        case DiagnosticCode::D_UnresolvedImport:         return "D_UnresolvedImport";
        case DiagnosticCode::D_UnresolvedReference:      return "D_UnresolvedReference";
    }
    return "Unknown";
}

std::string diagnosticCodePrefix(DiagnosticCode c) {
    // The numeric value carries the phase letter in its high nibble:
    //   0x0xxx → P0xxx     (parse)
    //   0x9xxx → P9xxx     (parse, internal-invariant range)
    //   0xCxxx → C0xxx     (config)
    //   0xDxxx → D0xxx     (driver / compilation-unit)
    // Render as the 4-digit hex grouping the user actually sees.
    const auto v          = static_cast<std::uint16_t>(c);
    const std::uint16_t nibble = v & 0xF000u;
    char letter = 'P';
    if (nibble == 0xC000u) {
        letter = 'C';
    } else if (nibble == 0xD000u) {
        letter = 'D';
    }
    // Strip the high nibble for the numeric portion when it's a phase
    // marker (C/D). The 9xxx range stays 9xxx so P_BuilderInvariant prints
    // as "P9000".
    const bool hasNibbleMarker = (nibble == 0xC000u || nibble == 0xD000u);
    const std::uint16_t lo = hasNibbleMarker ? (v & 0x0FFFu) : v;
    return std::format("{}{:04X}", letter, lo);
}

} // namespace dss
