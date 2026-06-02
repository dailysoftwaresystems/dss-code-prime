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
        case DiagnosticCode::C_UnknownTypeExtension:     return "C_UnknownTypeExtension";
        case DiagnosticCode::C_TypeExtensionParamMismatch: return "C_TypeExtensionParamMismatch";
        case DiagnosticCode::C_InvalidImports:           return "C_InvalidImports";
        case DiagnosticCode::C_MissingWrapperRules:      return "C_MissingWrapperRules";
        case DiagnosticCode::C_MissingNumberStyle:       return "C_MissingNumberStyle";
        case DiagnosticCode::C_InvalidNumberStyle:       return "C_InvalidNumberStyle";
        case DiagnosticCode::C_DuplicateWrapperRules:    return "C_DuplicateWrapperRules";
        case DiagnosticCode::C_InvalidSemantics:         return "C_InvalidSemantics";
        case DiagnosticCode::C_UnknownArtifactProfile:   return "C_UnknownArtifactProfile";
        case DiagnosticCode::C_InvalidHirLowering:       return "C_InvalidHirLowering";
        case DiagnosticCode::C_InvalidShippedFfiHeaderPath: return "C_InvalidShippedFfiHeaderPath";
        case DiagnosticCode::S_UndeclaredIdentifier:     return "S_UndeclaredIdentifier";
        case DiagnosticCode::S_RedeclaredSymbol:         return "S_RedeclaredSymbol";
        case DiagnosticCode::S_TypeMismatch:             return "S_TypeMismatch";
        case DiagnosticCode::S_NotCallable:              return "S_NotCallable";
        case DiagnosticCode::S_ArgCountMismatch:         return "S_ArgCountMismatch";
        case DiagnosticCode::S_UnknownType:              return "S_UnknownType";
        case DiagnosticCode::S_ConstViolation:           return "S_ConstViolation";
        case DiagnosticCode::S_ReturnTypeMismatch:       return "S_ReturnTypeMismatch";
        case DiagnosticCode::S_ControlOutsideLoop:       return "S_ControlOutsideLoop";
        case DiagnosticCode::S_UnusedVariable:           return "S_UnusedVariable";
        case DiagnosticCode::S_NonConstantArrayLength:   return "S_NonConstantArrayLength";
        case DiagnosticCode::S_NonConstantEnumeratorValue: return "S_NonConstantEnumeratorValue";
        case DiagnosticCode::S_ArrayLengthOutOfRange:    return "S_ArrayLengthOutOfRange";
        case DiagnosticCode::D_FileNotFound:             return "D_FileNotFound";
        case DiagnosticCode::D_EmptyInput:               return "D_EmptyInput";
        case DiagnosticCode::D_DuplicateFile:            return "D_DuplicateFile";
        case DiagnosticCode::D_UnresolvedImport:         return "D_UnresolvedImport";
        case DiagnosticCode::D_UnresolvedReference:      return "D_UnresolvedReference";
        case DiagnosticCode::D_UnknownFileExtension:     return "D_UnknownFileExtension";
        case DiagnosticCode::D_InvalidTargetSpec:        return "D_InvalidTargetSpec";
        case DiagnosticCode::D_SchemaLoadFailed:         return "D_SchemaLoadFailed";
        case DiagnosticCode::D_PlanNotLanded:            return "D_PlanNotLanded";
        case DiagnosticCode::D_OutputDirCreateFailed:    return "D_OutputDirCreateFailed";
        case DiagnosticCode::D_DirectoryScanFailed:      return "D_DirectoryScanFailed";
        case DiagnosticCode::H_TypeUnresolved:           return "H_TypeUnresolved";
        case DiagnosticCode::H_InvalidBreak:             return "H_InvalidBreak";
        case DiagnosticCode::H_VerifierFailure:          return "H_VerifierFailure";
        case DiagnosticCode::H_UnknownIntrinsic:         return "H_UnknownIntrinsic";
        case DiagnosticCode::H_ShaderViolation:          return "H_ShaderViolation";
        case DiagnosticCode::H_TextMalformed:            return "H_TextMalformed";
        case DiagnosticCode::H_TextVersionMismatch:      return "H_TextVersionMismatch";
        case DiagnosticCode::H_TextUnknownName:          return "H_TextUnknownName";
        case DiagnosticCode::H_UnsupportedLoweringForKind: return "H_UnsupportedLoweringForKind";
        case DiagnosticCode::H_ExternHasInitializer:     return "H_ExternHasInitializer";
        case DiagnosticCode::H_ExternDeclMalformed:      return "H_ExternDeclMalformed";
        case DiagnosticCode::I_VerifierFailure:          return "I_VerifierFailure";
        case DiagnosticCode::I_NoEntryBlock:             return "I_NoEntryBlock";
        case DiagnosticCode::I_MultipleEntryBlocks:      return "I_MultipleEntryBlocks";
        case DiagnosticCode::I_EntryBlockNotFirst:       return "I_EntryBlockNotFirst";
        case DiagnosticCode::I_BlockNotTerminated:       return "I_BlockNotTerminated";
        case DiagnosticCode::I_PhiPredNotInCfg:          return "I_PhiPredNotInCfg";
        case DiagnosticCode::I_NotDominated:             return "I_NotDominated";
        case DiagnosticCode::I_TerminatorTypeMismatch:   return "I_TerminatorTypeMismatch";
        case DiagnosticCode::I_ArgIndexOutOfRange:       return "I_ArgIndexOutOfRange";
        case DiagnosticCode::I_ExtensionTypeInMir:       return "I_ExtensionTypeInMir";
        case DiagnosticCode::I_StructCfMismatch:         return "I_StructCfMismatch";
        case DiagnosticCode::I_UnreachableBlock:         return "I_UnreachableBlock";
        case DiagnosticCode::I_TextMalformed:            return "I_TextMalformed";
        case DiagnosticCode::I_TextVersionMismatch:      return "I_TextVersionMismatch";
        case DiagnosticCode::I_TextUnknownName:          return "I_TextUnknownName";
        case DiagnosticCode::L_UnsupportedLoweringForOpcode: return "L_UnsupportedLoweringForOpcode";
        case DiagnosticCode::L_RequiredLirOpcodeMissing:     return "L_RequiredLirOpcodeMissing";
        case DiagnosticCode::L_VirtualRegInPostRegalloc:     return "L_VirtualRegInPostRegalloc";
        case DiagnosticCode::L_InvalidSpillSlotSentinel:     return "L_InvalidSpillSlotSentinel";
        case DiagnosticCode::L_PhysRegOrdinalOutOfRange:     return "L_PhysRegOrdinalOutOfRange";
        case DiagnosticCode::L_MemOperandMalformed:          return "L_MemOperandMalformed";
        case DiagnosticCode::L_StackPassedArgUnsupported:    return "L_StackPassedArgUnsupported";
        case DiagnosticCode::L_CcRegLookupFailed:            return "L_CcRegLookupFailed";
        case DiagnosticCode::L_MoveCycleUnsupported:         return "L_MoveCycleUnsupported";
        case DiagnosticCode::L_IndirectCallUnsupported:      return "L_IndirectCallUnsupported";
        case DiagnosticCode::R_NoCallingConventions:          return "R_NoCallingConventions";
        case DiagnosticCode::R_CallingConventionLookupFailed: return "R_CallingConventionLookupFailed";
        case DiagnosticCode::R_VRegHasNoClass:                return "R_VRegHasNoClass";
        case DiagnosticCode::R_SpilledDueToPressure:          return "R_SpilledDueToPressure";
        case DiagnosticCode::R_SpilledDueToCrossCallExhaustion: return "R_SpilledDueToCrossCallExhaustion";
        case DiagnosticCode::A_NoEncodingDeclared:           return "A_NoEncodingDeclared";
        case DiagnosticCode::A_NoEncodingShapeWalker:        return "A_NoEncodingShapeWalker";
        case DiagnosticCode::A_LirToMirSizeMismatch:         return "A_LirToMirSizeMismatch";
        case DiagnosticCode::A_NoMatchingEncodingVariant:    return "A_NoMatchingEncodingVariant";
        case DiagnosticCode::A_RoundTripMismatch:            return "A_RoundTripMismatch";
        case DiagnosticCode::K_SymbolUndefined:              return "K_SymbolUndefined";
        case DiagnosticCode::K_RelocationKindMismatch:       return "K_RelocationKindMismatch";
        case DiagnosticCode::K_NoMatchingObjectFormat:       return "K_NoMatchingObjectFormat";
        case DiagnosticCode::K_FormatLacksImportSupport:     return "K_FormatLacksImportSupport";
        case DiagnosticCode::K_WalkerInputContractViolation: return "K_WalkerInputContractViolation";
        case DiagnosticCode::K_ImageNotOk:                   return "K_ImageNotOk";
        case DiagnosticCode::K_ImageWriteParentMissing:      return "K_ImageWriteParentMissing";
        case DiagnosticCode::K_ImageWriteOpenFailed:         return "K_ImageWriteOpenFailed";
        case DiagnosticCode::K_ImageWriteShort:              return "K_ImageWriteShort";
        case DiagnosticCode::K_ImageWriteCloseFailed:        return "K_ImageWriteCloseFailed";
        case DiagnosticCode::K_ImageEmpty:                   return "K_ImageEmpty";
        case DiagnosticCode::K_ChainedFixupsNotYetIntegrated: return "K_ChainedFixupsNotYetIntegrated";

        case DiagnosticCode::D_TargetFormatMismatch:         return "D_TargetFormatMismatch";
        case DiagnosticCode::D_TargetMachineCodeMismatch:    return "D_TargetMachineCodeMismatch";
        case DiagnosticCode::D_TargetAbiModelMismatch:       return "D_TargetAbiModelMismatch";
        case DiagnosticCode::D_TargetAbiModelUnsupportedByDriver: return "D_TargetAbiModelUnsupportedByDriver";

        case DiagnosticCode::F_FileOpenFailed:               return "F_FileOpenFailed";
        case DiagnosticCode::F_FileEmpty:                    return "F_FileEmpty";
        case DiagnosticCode::F_UnknownBinaryFormat:          return "F_UnknownBinaryFormat";
        case DiagnosticCode::F_UnsupportedBinaryFormat:      return "F_UnsupportedBinaryFormat";
        case DiagnosticCode::F_CorruptedBinary:              return "F_CorruptedBinary";
        case DiagnosticCode::F_UnsupportedElfClass:          return "F_UnsupportedElfClass";
        case DiagnosticCode::F_SectionNotFound:              return "F_SectionNotFound";
        case DiagnosticCode::F_HeaderParseFailed:            return "F_HeaderParseFailed";
        case DiagnosticCode::F_HeaderHasFunctionBody:        return "F_HeaderHasFunctionBody";
        case DiagnosticCode::F_HeaderHasNonExternDecl:       return "F_HeaderHasNonExternDecl";
        case DiagnosticCode::F_HeaderEmptyImportLibrary:     return "F_HeaderEmptyImportLibrary";
        case DiagnosticCode::F_HeaderGrammarLoadFailed:      return "F_HeaderGrammarLoadFailed";
        case DiagnosticCode::F_HeaderHasUnsupportedTopLevel: return "F_HeaderHasUnsupportedTopLevel";
        case DiagnosticCode::F_HeaderInternalInvariant:      return "F_HeaderInternalInvariant";
        case DiagnosticCode::F_HeaderInvalidShippedPath:     return "F_HeaderInvalidShippedPath";
        case DiagnosticCode::F_AbiUnknownTuple:              return "F_AbiUnknownTuple";
        case DiagnosticCode::F_AbiNoMatchingCcInTarget:      return "F_AbiNoMatchingCcInTarget";
        case DiagnosticCode::F_AbiFormatAbiModelMismatch:    return "F_AbiFormatAbiModelMismatch";
        case DiagnosticCode::F_AbiCcRegistersInconsistent:   return "F_AbiCcRegistersInconsistent";
        case DiagnosticCode::F_MangleMissingExpectedPrefix:  return "F_MangleMissingExpectedPrefix";
        case DiagnosticCode::F_FfiIngestDuplicateSymbol:     return "F_FfiIngestDuplicateSymbol";
        case DiagnosticCode::F_FfiIngestAbiModelUnsupported: return "F_FfiIngestAbiModelUnsupported";
        case DiagnosticCode::F_FfiIngestEmptyCanonical:      return "F_FfiIngestEmptyCanonical";
        case DiagnosticCode::F_BinaryReaderPartialCorruption: return "F_BinaryReaderPartialCorruption";
    }
    return "Unknown";
}

std::string diagnosticCodePrefix(DiagnosticCode c) {
    // The numeric value carries the phase letter in its high nibble.
    // CROSS-PLAN AUTHORITY: this table mirrors plan 00 §0.3 — the two
    // sources MUST stay in lockstep. When adding a new family, update
    // BOTH in the same PR.
    //   0x0xxx → P0xxx     (parse)
    //   0x1xxx → A0xxx     (assembler — plan 13 AS1; allocated 2026-05-29)
    //   0x4xxx → R0xxx     (register allocator)
    //   0x5xxx → O0xxx     RESERVED — object format / linker (plan 14;
    //                       holding the slot so plan-14 doesn't accidentally
    //                       land on 0xCxxx (which is C_*) or 0xDxxx (D_*))
    //   0x5xxx → F0xxx     (FFI binary-reader + C-header-parser; plan 11 §2.6 — allocated 2026-06-01)
    //   0x6xxx → W0xxx     RESERVED — WAT/WASM verifier (plan 18; allocated 2026-05-29)
    //   0x7xxx → V0xxx     RESERVED — SPIR-V verifier  (plan 17; allocated 2026-05-29)
    //   0x9xxx → P9xxx     (parse, internal-invariant range)
    //   0xCxxx → C0xxx     (config)
    //   0xDxxx → D0xxx     (driver / compilation-unit)
    //   0xAxxx → I0xxx     (MIR verifier / IR-gen mid-level)
    //   0xBxxx → L0xxx     (LIR lowering + verifier)
    //   0x8xxx → K0xxx     (linker — plan 14 LK4)
    //   0xExxx → S0xxx     (semantic analysis)
    //   0xFxxx → H0xxx     (HIR verifier / lowering)
    // Free for future families: 0x2xxx, 0x3xxx (reserve for JVM IL /
    // .NET IL / future shader-stage validators post-v1).
    // Render as the 4-digit hex grouping the user actually sees.
    const auto v          = static_cast<std::uint16_t>(c);
    const std::uint16_t nibble = v & 0xF000u;
    char letter = 'P';
    if (nibble == 0x1000u) {
        letter = 'A';
    } else if (nibble == 0x4000u) {
        letter = 'R';
    } else if (nibble == 0x5000u) {
        letter = 'F';
    } else if (nibble == 0x8000u) {
        letter = 'K';
    } else if (nibble == 0xA000u) {
        letter = 'I';
    } else if (nibble == 0xB000u) {
        letter = 'L';
    } else if (nibble == 0xC000u) {
        letter = 'C';
    } else if (nibble == 0xD000u) {
        letter = 'D';
    } else if (nibble == 0xE000u) {
        letter = 'S';
    } else if (nibble == 0xF000u) {
        letter = 'H';
    }
    // Strip the high nibble for the numeric portion when it's a phase
    // marker (A/K/R/C/D/S/H/I/L). The 9xxx range stays 9xxx so
    // P_BuilderInvariant prints as "P9000".
    const bool hasNibbleMarker = (nibble == 0x1000u || nibble == 0x4000u
                                  || nibble == 0x5000u
                                  || nibble == 0x8000u
                                  || nibble == 0xA000u || nibble == 0xB000u
                                  || nibble == 0xC000u || nibble == 0xD000u
                                  || nibble == 0xE000u || nibble == 0xF000u);
    const std::uint16_t lo = hasNibbleMarker ? (v & 0x0FFFu) : v;
    return std::format("{}{:04X}", letter, lo);
}

} // namespace dss
