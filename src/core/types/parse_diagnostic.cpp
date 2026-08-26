#include "core/types/parse_diagnostic.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <string_view>

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
        case DiagnosticCode::P_PreprocessorDirective:    return "P_PreprocessorDirective";
        case DiagnosticCode::P_PreprocessorMacroRedefinition: return "P_PreprocessorMacroRedefinition";
        case DiagnosticCode::P_PreprocessorUnsupported:  return "P_PreprocessorUnsupported";
        case DiagnosticCode::P_PreprocessorIncludeError: return "P_PreprocessorIncludeError";
        case DiagnosticCode::P_PreprocessorMacroArgument: return "P_PreprocessorMacroArgument";
        case DiagnosticCode::P_PreprocessorStringize:    return "P_PreprocessorStringize";
        case DiagnosticCode::P_PreprocessorPaste:        return "P_PreprocessorPaste";
        case DiagnosticCode::P_PreprocessorPredefinedMacro: return "P_PreprocessorPredefinedMacro";
        case DiagnosticCode::P_PreprocessorHasInclude:   return "P_PreprocessorHasInclude";
        case DiagnosticCode::P_PreprocessorEmbed:        return "P_PreprocessorEmbed";
        case DiagnosticCode::P_PreprocessorErrorDirective:
            return "P_PreprocessorErrorDirective";
        case DiagnosticCode::P_PreprocessorWarningDirective:
            return "P_PreprocessorWarningDirective";
        case DiagnosticCode::P_PreprocessorPragma:
            return "P_PreprocessorPragma";
        case DiagnosticCode::P_PreprocessorOperatorNameNotDefinable:
            return "P_PreprocessorOperatorNameNotDefinable";
        case DiagnosticCode::P_PreprocessorIncludeReentryRefused:
            return "P_PreprocessorIncludeReentryRefused";
        case DiagnosticCode::S_PragmaPackAmbiguous:
            return "S_PragmaPackAmbiguous";
        case DiagnosticCode::S_AsmLabelInvalid:
            return "S_AsmLabelInvalid";
        case DiagnosticCode::S_AsmLabelDuplicate:
            return "S_AsmLabelDuplicate";
        case DiagnosticCode::S_AsmLabelOnAutomaticVariable:
            return "S_AsmLabelOnAutomaticVariable";
        case DiagnosticCode::S_AttributeIgnoredForDeclarationKind:
            return "S_AttributeIgnoredForDeclarationKind";
        case DiagnosticCode::S_IncompatiblePointerIntegerPointee:
            return "S_IncompatiblePointerIntegerPointee";
        case DiagnosticCode::S_EntryShapeNotDeclared:
            return "S_EntryShapeNotDeclared";
        case DiagnosticCode::S_InlineAsmExtendedUnsupported:
            return "S_InlineAsmExtendedUnsupported";
        case DiagnosticCode::S_InlineAsmLabelSectionRequiresGoto:
            return "S_InlineAsmLabelSectionRequiresGoto";
        case DiagnosticCode::S_InlineAsmDuplicateQualifier:
            return "S_InlineAsmDuplicateQualifier";
        // Inline-asm P5 operand binding, 0xE065..0xE06B.
        case DiagnosticCode::S_InlineAsmConstraintLetterUndeclared:
            return "S_InlineAsmConstraintLetterUndeclared";
        case DiagnosticCode::S_InlineAsmConstraintUnsupportedForm:
            return "S_InlineAsmConstraintUnsupportedForm";
        case DiagnosticCode::S_InlineAsmOperandModifierUnsupported:
            return "S_InlineAsmOperandModifierUnsupported";
        case DiagnosticCode::S_InlineAsmClobberUnknown:
            return "S_InlineAsmClobberUnknown";
        case DiagnosticCode::S_InlineAsmTemplateUnparsable:
            return "S_InlineAsmTemplateUnparsable";
        case DiagnosticCode::S_InlineAsmPlaceholderOutOfRange:
            return "S_InlineAsmPlaceholderOutOfRange";
        case DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate:
            return "S_InlineAsmPlaceholderInBasicTemplate";
        case DiagnosticCode::S_InlineAsmDuplicateSymbolicName:
            return "S_InlineAsmDuplicateSymbolicName";
        case DiagnosticCode::S_OffsetofInvalidMember:
            return "S_OffsetofInvalidMember";
        case DiagnosticCode::S_BuiltinChooseExprNonConstant:
            return "S_BuiltinChooseExprNonConstant";
        case DiagnosticCode::S_StatementExprHasNoValue:
            return "S_StatementExprHasNoValue";
        case DiagnosticCode::S_StatementExprAtFileScope:
            return "S_StatementExprAtFileScope";
        case DiagnosticCode::S_VlaInitializerNotEmpty:
            return "S_VlaInitializerNotEmpty";
        case DiagnosticCode::P_ExpressionTooDeep:        return "P_ExpressionTooDeep";
        case DiagnosticCode::P_BuilderInvariant:         return "P_BuilderInvariant";
        case DiagnosticCode::P_TooManyDiagnostics:       return "P_TooManyDiagnostics";
        case DiagnosticCode::P_UnfinishedTree:           return "P_UnfinishedTree";
        case DiagnosticCode::P_RecoveryStalled:          return "P_RecoveryStalled";
        case DiagnosticCode::P_MaxSpeculationDepth:      return "P_MaxSpeculationDepth";
        case DiagnosticCode::P_UncommittedCheckpoint:    return "P_UncommittedCheckpoint";
        case DiagnosticCode::P_BacktrackFailed:          return "P_BacktrackFailed";
        case DiagnosticCode::P_DiagnosticsElided:        return "P_DiagnosticsElided";
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
        case DiagnosticCode::C_InvalidTargetName:        return "C_InvalidTargetName";
        case DiagnosticCode::C_InvalidFormatName:        return "C_InvalidFormatName";
        case DiagnosticCode::C_InvalidPreprocess:        return "C_InvalidPreprocess";
        case DiagnosticCode::C_ConflictingPredefinedMacro: return "C_ConflictingPredefinedMacro";
        case DiagnosticCode::C_UnbackedPredefinedMacro: return "C_UnbackedPredefinedMacro";
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
        case DiagnosticCode::S_InvalidCast:              return "S_InvalidCast";
        case DiagnosticCode::S_InvalidTypeSpecifierCombination:
            return "S_InvalidTypeSpecifierCombination";
        case DiagnosticCode::S_IntegerLiteralTooLarge:   return "S_IntegerLiteralTooLarge";
        case DiagnosticCode::S_UnsupportedDataModel:     return "S_UnsupportedDataModel";
        case DiagnosticCode::S_VolatileNotSupported:     return "S_VolatileNotSupported";
        case DiagnosticCode::S_IndirectCallNotSupported: return "S_IndirectCallNotSupported";
        case DiagnosticCode::S_InvalidVoidParam:         return "S_InvalidVoidParam";
        case DiagnosticCode::S_DeclarationDeclaresNothing:
            return "S_DeclarationDeclaresNothing";
        case DiagnosticCode::S_InvalidFunctionDeclarator:
            return "S_InvalidFunctionDeclarator";
        case DiagnosticCode::S_DuplicateLabel:           return "S_DuplicateLabel";
        case DiagnosticCode::S_UndefinedLabel:           return "S_UndefinedLabel";
        case DiagnosticCode::S_FlexibleArrayNotLast:     return "S_FlexibleArrayNotLast";
        case DiagnosticCode::S_FlexibleArraySoleMember:  return "S_FlexibleArraySoleMember";
        case DiagnosticCode::S_FlexibleArrayInAggregate: return "S_FlexibleArrayInAggregate";
        case DiagnosticCode::S_BitFieldNonIntegerType:   return "S_BitFieldNonIntegerType";
        case DiagnosticCode::S_BitFieldWidthOutOfRange:  return "S_BitFieldWidthOutOfRange";
        case DiagnosticCode::S_VariadicCalleeUnsupported: return "S_VariadicCalleeUnsupported";
        case DiagnosticCode::S_StaticStorageInForInit:   return "S_StaticStorageInForInit";
        case DiagnosticCode::S_IncompatibleRedeclaration: return "S_IncompatibleRedeclaration";
        case DiagnosticCode::S_CaseLabelNotInSwitch:     return "S_CaseLabelNotInSwitch";
        case DiagnosticCode::S_IncDecNeedsModifiableLvalue: return "S_IncDecNeedsModifiableLvalue";
        case DiagnosticCode::S_VolatilePointeeNotSupported:
            return "S_VolatilePointeeNotSupported";
        case DiagnosticCode::S_IncompleteTypeMember:
            return "S_IncompleteTypeMember";
        case DiagnosticCode::S_TypeNameDeclaratorNotAbstract:
            return "S_TypeNameDeclaratorNotAbstract";
        case DiagnosticCode::S_IncompleteTypeObject:
            return "S_IncompleteTypeObject";
        case DiagnosticCode::S_StaticAssertFailed:
            return "S_StaticAssertFailed";
        case DiagnosticCode::S_GenericSelectionNoMatch:
            return "S_GenericSelectionNoMatch";
        case DiagnosticCode::S_GenericSelectionAmbiguous:
            return "S_GenericSelectionAmbiguous";
        case DiagnosticCode::S_AlignasNotPowerOfTwo:
            return "S_AlignasNotPowerOfTwo";
        case DiagnosticCode::S_AlignasExceedsMax:
            return "S_AlignasExceedsMax";
        case DiagnosticCode::S_AlignasWeakerThanNatural:
            return "S_AlignasWeakerThanNatural";
        case DiagnosticCode::S_AlignasInvalidContext:
            return "S_AlignasInvalidContext";
        case DiagnosticCode::S_AlignasNonConstant:
            return "S_AlignasNonConstant";
        case DiagnosticCode::S_UnknownTypeAttribute:
            return "S_UnknownTypeAttribute";
        case DiagnosticCode::S_PackedBitfieldUnsupported:
            return "S_PackedBitfieldUnsupported";
        case DiagnosticCode::S_NullptrInvalidOperand:
            return "S_NullptrInvalidOperand";
        case DiagnosticCode::S_InvalidEnumUnderlyingType:
            return "S_InvalidEnumUnderlyingType";
        case DiagnosticCode::S_EnumeratorValueOutOfRange:
            return "S_EnumeratorValueOutOfRange";
        case DiagnosticCode::S_TypeofBitfieldOperand:
            return "S_TypeofBitfieldOperand";
        case DiagnosticCode::S_ConstexprNonConstantInitializer:
            return "S_ConstexprNonConstantInitializer";
        case DiagnosticCode::S_ConstexprMissingInitializer:
            return "S_ConstexprMissingInitializer";
        case DiagnosticCode::S_ConstexprUnsupportedType:
            return "S_ConstexprUnsupportedType";
        case DiagnosticCode::S_ConstexprFunctionNotSupported:
            return "S_ConstexprFunctionNotSupported";
        case DiagnosticCode::S_ConstexprInvalidQualifier:
            return "S_ConstexprInvalidQualifier";
        case DiagnosticCode::S_UnknownAttribute:
            return "S_UnknownAttribute";
        case DiagnosticCode::S_DeprecatedSymbolUsed:
            return "S_DeprecatedSymbolUsed";
        case DiagnosticCode::S_NodiscardResultDiscarded:
            return "S_NodiscardResultDiscarded";
        case DiagnosticCode::S_InvalidScalarInitializer:
            return "S_InvalidScalarInitializer";
        case DiagnosticCode::S_PredefinedIdentifierNotAddressable:
            return "S_PredefinedIdentifierNotAddressable";
        case DiagnosticCode::S_AutoRequiresSingleDeclarator:
            return "S_AutoRequiresSingleDeclarator";
        case DiagnosticCode::S_AutoRequiresPlainIdentifier:
            return "S_AutoRequiresPlainIdentifier";
        case DiagnosticCode::S_AutoRequiresInitializer:
            return "S_AutoRequiresInitializer";
        case DiagnosticCode::S_AutoInferenceInvalid:
            return "S_AutoInferenceInvalid";
        case DiagnosticCode::S_ThreadLocalOnFunction:
            return "S_ThreadLocalOnFunction";
        case DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern:
            return "S_ThreadLocalRequiresStaticOrExtern";
        case DiagnosticCode::S_ThreadLocalRedeclarationMismatch:
            return "S_ThreadLocalRedeclarationMismatch";
        case DiagnosticCode::S_ThreadLocalAddressNotConstant:
            return "S_ThreadLocalAddressNotConstant";
        case DiagnosticCode::S_ThreadLocalInvalidCombination:
            return "S_ThreadLocalInvalidCombination";
        case DiagnosticCode::S_BitIntWidthNotConstant:
            return "S_BitIntWidthNotConstant";
        case DiagnosticCode::S_BitIntWidthNotPositive:
            return "S_BitIntWidthNotPositive";
        case DiagnosticCode::S_BitIntSignedWidthTooSmall:
            return "S_BitIntSignedWidthTooSmall";
        case DiagnosticCode::S_BitIntWidthExceedsMax:
            return "S_BitIntWidthExceedsMax";
        case DiagnosticCode::S_BitIntWidthAboveC1Limit:
            return "S_BitIntWidthAboveC1Limit";
        case DiagnosticCode::S_BitIntWideMulDivUnsupported:
            return "S_BitIntWideMulDivUnsupported";
        case DiagnosticCode::S_BitIntWideFloatConvUnsupported:
            return "S_BitIntWideFloatConvUnsupported";
        case DiagnosticCode::S_VlaWithStaticStorage:     return "S_VlaWithStaticStorage";
        case DiagnosticCode::S_VlaMultiDimUnsupported:   return "S_VlaMultiDimUnsupported";
        case DiagnosticCode::S_VlaSizeNotInteger:        return "S_VlaSizeNotInteger";
        case DiagnosticCode::S_ArrayParamQualifierNonParameter:
            return "S_ArrayParamQualifierNonParameter";
        case DiagnosticCode::S_AtomicNonLockFree:        return "S_AtomicNonLockFree";
        case DiagnosticCode::S_LongDoubleFormatUndeclared:
            return "S_LongDoubleFormatUndeclared";
        case DiagnosticCode::S_InlineAsmNonEmptyTemplate:
            return "S_InlineAsmNonEmptyTemplate";
        case DiagnosticCode::S_BitfieldMutationUnsupportedBase:
            return "S_BitfieldMutationUnsupportedBase";
        case DiagnosticCode::S_InlineNonFunction:
            return "S_InlineNonFunction";
        case DiagnosticCode::S_ConflictingInlineAttributes:
            return "S_ConflictingInlineAttributes";   // TF-C81
        case DiagnosticCode::D_FileNotFound:             return "D_FileNotFound";
        case DiagnosticCode::D_FileReadFailed:           return "D_FileReadFailed";
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
        case DiagnosticCode::H_UnknownLinkageSpecifier:  return "H_UnknownLinkageSpecifier";
        case DiagnosticCode::H_UnreachableCode:          return "H_UnreachableCode";
        case DiagnosticCode::H_SehBuiltinContext:        return "H_SehBuiltinContext";
        case DiagnosticCode::H_SehJumpIntoRegion:        return "H_SehJumpIntoRegion";
        case DiagnosticCode::H_SehEarlyExit:             return "H_SehEarlyExit";
        case DiagnosticCode::H_SehLabelAddress:          return "H_SehLabelAddress";
        case DiagnosticCode::H_WideCharSurrogateUnsupported: return "H_WideCharSurrogateUnsupported";
        case DiagnosticCode::H_Utf8CharLiteralOutOfRange: return "H_Utf8CharLiteralOutOfRange";
        case DiagnosticCode::H_WideCharValueUnrepresentable: return "H_WideCharValueUnrepresentable";
        case DiagnosticCode::H_InvalidUniversalCharacterName: return "H_InvalidUniversalCharacterName";
        case DiagnosticCode::H_WideByteEscapeUnsupported: return "H_WideByteEscapeUnsupported";
        case DiagnosticCode::H_ConflictingStringLiteralPrefixes: return "H_ConflictingStringLiteralPrefixes";
        case DiagnosticCode::H_VlaJumpIntoScope:         return "H_VlaJumpIntoScope";
        case DiagnosticCode::H_VlaComputedGotoInScope:   return "H_VlaComputedGotoInScope";
        case DiagnosticCode::H_ShippedShimSignatureMismatch:
            return "H_ShippedShimSignatureMismatch";
        case DiagnosticCode::I_VerifierFailure:          return "I_VerifierFailure";
        case DiagnosticCode::I_NoEntryBlock:             return "I_NoEntryBlock";
        case DiagnosticCode::I_MultipleEntryBlocks:      return "I_MultipleEntryBlocks";
        case DiagnosticCode::I_EntryBlockNotFirst:       return "I_EntryBlockNotFirst";
        case DiagnosticCode::I_BlockNotTerminated:       return "I_BlockNotTerminated";
        case DiagnosticCode::I_PhiPredNotInCfg:          return "I_PhiPredNotInCfg";
        case DiagnosticCode::I_NotDominated:             return "I_NotDominated";
        case DiagnosticCode::I_TerminatorTypeMismatch:   return "I_TerminatorTypeMismatch";
        case DiagnosticCode::I_ArgIndexOutOfRange:       return "I_ArgIndexOutOfRange";
        case DiagnosticCode::I_ArgPositionDuplicate:     return "I_ArgPositionDuplicate";
        case DiagnosticCode::I_AllocaAlignmentNotPowerOfTwo: return "I_AllocaAlignmentNotPowerOfTwo";
        case DiagnosticCode::I_NullptrTypeInMir:         return "I_NullptrTypeInMir";
        case DiagnosticCode::I_BitIntWidthInconsistent:  return "I_BitIntWidthInconsistent";
        case DiagnosticCode::I_VlaAllocaOperandInvalid:  return "I_VlaAllocaOperandInvalid";
        case DiagnosticCode::I_VlaStackRestorePairing:   return "I_VlaStackRestorePairing";
        case DiagnosticCode::I_AtomicAccessNotLowered:   return "I_AtomicAccessNotLowered";
        case DiagnosticCode::I_CallSignatureMismatch:    return "I_CallSignatureMismatch";
        case DiagnosticCode::I_StoreValueTypeMismatch:   return "I_StoreValueTypeMismatch";
        case DiagnosticCode::I_ExtensionTypeInMir:       return "I_ExtensionTypeInMir";
        case DiagnosticCode::I_StructCfMismatch:         return "I_StructCfMismatch";
        case DiagnosticCode::I_UnreachableBlock:         return "I_UnreachableBlock";
        case DiagnosticCode::I_TextMalformed:            return "I_TextMalformed";
        case DiagnosticCode::I_TextVersionMismatch:      return "I_TextVersionMismatch";
        case DiagnosticCode::I_TextUnknownName:          return "I_TextUnknownName";
        case DiagnosticCode::I_LayoutUseBeforeDef:       return "I_LayoutUseBeforeDef";
        case DiagnosticCode::I_SehStructure:             return "I_SehStructure";
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
        case DiagnosticCode::L_IndirectCalleeClobberedByArgSetup: return "L_IndirectCalleeClobberedByArgSetup";
        case DiagnosticCode::L_OverAlignedStackLocal:        return "L_OverAlignedStackLocal";
        case DiagnosticCode::L_VlaDynamicAllocaUnsupported:  return "L_VlaDynamicAllocaUnsupported";
        case DiagnosticCode::L_VlaNonLeafFrameUnsupported:   return "L_VlaNonLeafFrameUnsupported";
        case DiagnosticCode::L_TerminatorSuccessorMismatch:  return "L_TerminatorSuccessorMismatch";
        case DiagnosticCode::L_SideStructureIndexDangling:   return "L_SideStructureIndexDangling";
        case DiagnosticCode::L_SideStructurePoolShrank:      return "L_SideStructurePoolShrank";
        case DiagnosticCode::L_SideStructureReferenceLost:   return "L_SideStructureReferenceLost";
        case DiagnosticCode::L_ArgClassHasNoRegisterPool:    return "L_ArgClassHasNoRegisterPool";
        case DiagnosticCode::L_ArgClassPoolUndeclared:       return "L_ArgClassPoolUndeclared";
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
        case DiagnosticCode::A_ImmediateOperandOutOfRange:   return "A_ImmediateOperandOutOfRange";
        case DiagnosticCode::A_ImmediateNarrowedToOperandField: return "A_ImmediateNarrowedToOperandField";
        case DiagnosticCode::A_AsmTextUnsupported:           return "A_AsmTextUnsupported";
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
        case DiagnosticCode::K_EntryPointResolvesToExtern:   return "K_EntryPointResolvesToExtern";
        case DiagnosticCode::K_DuplicateDataSymbol:          return "K_DuplicateDataSymbol";
        case DiagnosticCode::K_BssDataHasBytes:              return "K_BssDataHasBytes";
        case DiagnosticCode::K_CrossCuMergeUnsupported:      return "K_CrossCuMergeUnsupported";
        case DiagnosticCode::K_SymbolRedefinedAcrossUnits:   return "K_SymbolRedefinedAcrossUnits";
        case DiagnosticCode::K_CrossCuImageEmitDeferred:     return "K_CrossCuImageEmitDeferred";

        case DiagnosticCode::D_TargetFormatMismatch:         return "D_TargetFormatMismatch";
        case DiagnosticCode::D_TargetMachineCodeMismatch:    return "D_TargetMachineCodeMismatch";
        case DiagnosticCode::D_TargetAbiModelMismatch:       return "D_TargetAbiModelMismatch";
        case DiagnosticCode::D_TargetAbiModelUnsupportedByDriver: return "D_TargetAbiModelUnsupportedByDriver";
        case DiagnosticCode::D_ArtifactProfileNotSupported:  return "D_ArtifactProfileNotSupported";
        case DiagnosticCode::D_ArtifactProfileFormatMismatch: return "D_ArtifactProfileFormatMismatch";
        case DiagnosticCode::D_DefineRequiresPreprocess:     return "D_DefineRequiresPreprocess";
        case DiagnosticCode::D_StaticLibFatArchiveUnsupported: return "D_StaticLibFatArchiveUnsupported";
        case DiagnosticCode::D_CompileUnitNullNoDiagnostic:  return "D_CompileUnitNullNoDiagnostic";
        case DiagnosticCode::D_ArtifactNameEscapesOutputDir: return "D_ArtifactNameEscapesOutputDir";
        case DiagnosticCode::D_SynthRecipeFamilyUnknown:     return "D_SynthRecipeFamilyUnknown";
        // Project-manifest build hooks (`preBuildScripts` / `postBuildScripts`).
        case DiagnosticCode::D_ScriptSpawnFailed:            return "D_ScriptSpawnFailed";
        case DiagnosticCode::D_ScriptExitedNonZero:          return "D_ScriptExitedNonZero";
        // Project dependencies (`dependsOn`).
        case DiagnosticCode::D_DependencyManifestNotFound:   return "D_DependencyManifestNotFound";
        case DiagnosticCode::D_DependencyCycle:              return "D_DependencyCycle";
        case DiagnosticCode::D_DependencyArtifactProfileUnsupported:
            return "D_DependencyArtifactProfileUnsupported";
        case DiagnosticCode::D_DependencyLanguageMismatch:   return "D_DependencyLanguageMismatch";
        case DiagnosticCode::D_DependencyGitNotFound:        return "D_DependencyGitNotFound";
        case DiagnosticCode::D_DependencyGitAcquireFailed:   return "D_DependencyGitAcquireFailed";
        case DiagnosticCode::D_DependencyGitFetchFallback:   return "D_DependencyGitFetchFallback";
        case DiagnosticCode::D_DependencyGitNameCollision:   return "D_DependencyGitNameCollision";
        case DiagnosticCode::D_SuppressRequestIgnored:       return "D_SuppressRequestIgnored";
        // Project dependencies, continued (0xD022..0xD024) — the run is not
        // contiguous with the block above; 0xD021 landed between the halves.
        case DiagnosticCode::D_DependencyTargetFormatUnresolvable:
            return "D_DependencyTargetFormatUnresolvable";
        case DiagnosticCode::D_DependencyTargetFormatAmbiguous:
            return "D_DependencyTargetFormatAmbiguous";
        case DiagnosticCode::D_DependencyDerivedNameInvalid:
            return "D_DependencyDerivedNameInvalid";
        case DiagnosticCode::D_DependencyOutputNameCollision:
            return "D_DependencyOutputNameCollision";
        case DiagnosticCode::D_DependencyGraphTooDeep:
            return "D_DependencyGraphTooDeep";
        // The AP3 artifact-profile gate's reject split (the other arm is
        // D_ArtifactProfileFormatMismatch above).
        case DiagnosticCode::D_ArtifactProfileNoServingFormat:
            return "D_ArtifactProfileNoServingFormat";
        // The derivation SUCCEEDED and the dependency's own build then failed —
        // the fact 0xD022 used to absorb.
        case DiagnosticCode::D_DependencyBuildFailed:
            return "D_DependencyBuildFailed";
        // The language↔target architecture gate
        // (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE).
        case DiagnosticCode::D_LanguageTargetIsaMismatch:
            return "D_LanguageTargetIsaMismatch";

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
        case DiagnosticCode::F_FfiNoImportLibraryForFormat:  return "F_FfiNoImportLibraryForFormat";
        case DiagnosticCode::F_ShippedHeaderNotFound:        return "F_ShippedHeaderNotFound";
        case DiagnosticCode::F_ShippedLibDescriptorMalformed: return "F_ShippedLibDescriptorMalformed";
        case DiagnosticCode::F_ShippedLibUnsupportedType:    return "F_ShippedLibUnsupportedType";
        case DiagnosticCode::F_ShippedHeaderUnavailableForTarget:
            return "F_ShippedHeaderUnavailableForTarget";
        case DiagnosticCode::F_ShippedStructVariantAmbiguous:
            return "F_ShippedStructVariantAmbiguous";
        case DiagnosticCode::F_ShippedConstantVariantAmbiguous:
            return "F_ShippedConstantVariantAmbiguous";
        case DiagnosticCode::F_ShippedTypedefVariantAmbiguous:
            return "F_ShippedTypedefVariantAmbiguous";
        case DiagnosticCode::F_ShippedMacroVariantAmbiguous:
            return "F_ShippedMacroVariantAmbiguous";
        case DiagnosticCode::F_FfiResolveLibrarySymbolAbsent:
            return "F_FfiResolveLibrarySymbolAbsent";
        case DiagnosticCode::F_ShippedTypeIdentityConflict:
            return "F_ShippedTypeIdentityConflict";
        case DiagnosticCode::F_ShippedSymbolUnavailableForTarget:
            return "F_ShippedSymbolUnavailableForTarget";
        case DiagnosticCode::F_HeaderNameCaseAmbiguous:
            return "F_HeaderNameCaseAmbiguous";
        case DiagnosticCode::F_ShippedCorpusInvariantBroken:
            return "F_ShippedCorpusInvariantBroken";
        case DiagnosticCode::F_ObjectReaderSymbolBodyDropped:
            return "F_ObjectReaderSymbolBodyDropped";

        // Semantic (S_) + assembler (A_) + linker (K_) enumerators added in
        // later cycles but not mirrored here until the per-file -Werror=switch
        // gate (below) caught the drift.
        case DiagnosticCode::S_NotAComposite:                return "S_NotAComposite";
        case DiagnosticCode::S_NotAPointer:                  return "S_NotAPointer";
        case DiagnosticCode::A_FunctionEncodeAborted:        return "A_FunctionEncodeAborted";
        case DiagnosticCode::K_AbsolutePointerRelocMissing:  return "K_AbsolutePointerRelocMissing";
        case DiagnosticCode::K_ImageExecBitFailed:           return "K_ImageExecBitFailed";
        case DiagnosticCode::K_FormatLacksThreadLocalSupport:
            return "K_FormatLacksThreadLocalSupport";
        case DiagnosticCode::K_ThreadLocalOveralignedForFormat:
            return "K_ThreadLocalOveralignedForFormat";
        case DiagnosticCode::K_ArchiveMemberNameInvalid:
            return "K_ArchiveMemberNameInvalid";
        case DiagnosticCode::K_ArchiveFieldOverflow:
            return "K_ArchiveFieldOverflow";
        case DiagnosticCode::K_FormatLacksStackReserveControl:
            return "K_FormatLacksStackReserveControl";
        case DiagnosticCode::K_InvalidStackReserveRequest:
            return "K_InvalidStackReserveRequest";
        case DiagnosticCode::K_ExternImportAttributeConflict:
            return "K_ExternImportAttributeConflict";
        case DiagnosticCode::K_FormatLacksProcessExit:
            return "K_FormatLacksProcessExit";
        case DiagnosticCode::K_ExecEntryNotTrampolined:
            return "K_ExecEntryNotTrampolined";
        case DiagnosticCode::K_EntryVerbUnmaterializable:
            return "K_EntryVerbUnmaterializable";
        case DiagnosticCode::K_ProgramEntryUndefined:
            return "K_ProgramEntryUndefined";
        case DiagnosticCode::K_ProgramEntryAmbiguous:
            return "K_ProgramEntryAmbiguous";
        case DiagnosticCode::K_UnwindRuleUnrepresentable:
            return "K_UnwindRuleUnrepresentable";
        case DiagnosticCode::K_FormatLacksWeakDefinitionDialect:
            return "K_FormatLacksWeakDefinitionDialect";

        // Optimizer/pipeline (X_) family.
        case DiagnosticCode::X_UnknownPassId:                return "X_UnknownPassId";
        case DiagnosticCode::X_UnknownPassName:              return "X_UnknownPassName";
        case DiagnosticCode::X_PipelineVersionMismatch:      return "X_PipelineVersionMismatch";
        case DiagnosticCode::X_PipelineMalformed:            return "X_PipelineMalformed";
        case DiagnosticCode::X_PipelineNameResolutionFailed: return "X_PipelineNameResolutionFailed";
        case DiagnosticCode::X_OptPassSkipped:               return "X_OptPassSkipped";
        case DiagnosticCode::X_OptReturnFalseWithoutDiagnostic:
            return "X_OptReturnFalseWithoutDiagnostic";
        case DiagnosticCode::X_InlineMalformedCallSite:      return "X_InlineMalformedCallSite";
        case DiagnosticCode::X_OptFixpointTruncated:         return "X_OptFixpointTruncated";
    }
    // Not a `default:` arm — the switch above deliberately has none, so
    // `-Werror=switch` forces an arm for every enumerator and this line is
    // reachable ONLY for a 16-bit value that is not an enumerator at all.
    // That is what makes it the project's exact allocation oracle rather than
    // a fallback: see `kUnallocatedDiagnosticCodeName` in the header.
    return kUnallocatedDiagnosticCodeName;
}

namespace {

// One row per HIGH NIBBLE of the `DiagnosticCode` space. Sixteen rows, always,
// addressed BY INDEX (`kNibbleFamilies[v >> 12]`) rather than searched.
struct NibbleFamily {
    char             letter;        // kUnallocatedFamilyLetter iff unallocated
    bool             stripsNibble;  // render the low 12 bits only
    std::string_view owner;         // what the family is; documentation as data
};

// ★★★ D-DIAG-CODE-PREFIX-DEFAULT-IS-SILENT — WHY THIS IS A TABLE AND NOT A
// LADDER, AND WHY THE TABLE HAS SIXTEEN ROWS RATHER THAN ELEVEN.
//
// This function used to be an else-if ladder over the eleven ALLOCATED
// nibbles, seeded with `char letter = 'P'`. That seed was the defect. `'P'` is
// correct for exactly two nibbles (0x0 and 0x9, the parser's own ranges) and
// it was ALSO the answer for "I do not recognise this family" — so a family
// allocated in the header but not here rendered a PLAUSIBLE WRONG CODE rather
// than failing. That is not hypothetical: the whole X_* optimizer family
// shipped at 0x2xxx with no arm here and printed `P2002` for
// `X_PipelineVersionMismatch` — the parser's letter, the family nibble left
// un-stripped — for its entire life, and nothing noticed, because a
// wrong-but-well-formed diagnostic code passes every eye and every green
// suite. ✔MEASURED at the time: `grep -rInE 'P200[1-8]'` over src/ tests/
// examples/ real-examples/ docs/ .plans/ returned ZERO hits, so the rendering
// was not merely wrong, it was untested — no golden file had even recorded the
// wrong answer.
//
// ★ THE FIX IS STRUCTURAL, NOT A BETTER DEFAULT. Sixteen rows indexed by
// `v >> 12` means **there is no default arm to get wrong**: every possible
// high nibble already has a row, so "unhandled" is not a reachable state. A
// new family cannot half-land, because the row it needs already exists and
// says `kUnallocatedFamilyLetter` until someone fills it in.
//
// ★★ AND THE SECOND DUPLICATE IS GONE TOO. The old code carried the allocated
// set TWICE — once in the letter ladder and once in a `hasNibbleMarker`
// boolean OR-chain that decided nibble-stripping. Two hand-maintained copies
// of one fact, in one function, is the same drift hazard one scale down;
// `stripsNibble` is now a field of the single row.
//
// ⚠ WHAT THIS TABLE DOES NOT DO: it does not abort on an unallocated family.
// `hir_text.cpp`'s `@diag(code N)` handler deliberately renders a code it has
// already refused as unallocated, to show the operator what it would have
// looked like. Aborting would turn that refusal into a crash on a `.dsshir`
// input. The fail-loud obligation is met by `kUnallocatedFamilyLetter` (`?`),
// which no reader and no `[A-Z][0-9A-F]{4}` scraper can mistake for a code —
// plus the TOTALITY TEST in tests/core/test_parse_diagnostic.cpp
// (`EveryAllocatedCodeRendersUnderARealFamilyLetter`), which walks all 65536
// values, asks `diagnosticCodeName` whether each is allocated, and fails if any
// allocated code renders under `?`. That test is what makes half-landing a
// family a RED rather than a silent `P2002`; it is derived from the enum, not
// from a hand-listed sample, so it covers the NEXT family automatically.
//
// CROSS-PLAN AUTHORITY: plan 00 §0.3 carries the same allocation. Claiming a
// family means updating plan 00 §0.3 and THIS table. ⓘ The third mirror —
// `NIBBLE_LETTER` in scripts/corpus-census/corpus-census.py, which had drifted
// identically and was mis-attributing every X_* diagnostic to the parser in the
// very instrument whose job is to attribute failures to a tier — no longer
// hand-copies these rows; it parses them out of this table.
//
// ⚠ 0x5xxx IS CLAIMED TWICE IN PLAN 00 §0.3 (`O0xxx` RESERVED for the object
// format / linker, and `F0xxx` for the FFI reader). Only `F` was ever rendered
// and only `F_*` codes were ever allocated there, so this table records the
// SHIPPED truth rather than the plan's ambiguity. That conflict is
// D-DIAG-0X5XXX-NIBBLE-CLAIMED-BY-TWO-FAMILIES and is not resolved here.
constexpr std::array<NibbleFamily, 16> kNibbleFamilies{{
    /* 0x0 */ {'P', false, "parser"},
    /* 0x1 */ {'A', true,  "assembler (plan 13 AS1, allocated 2026-05-29)"},
    /* 0x2 */ {'X', true,  "optimizer pass engine + pass internals (plan 22 PR1)"},
    /* 0x3 */ {kUnallocatedFamilyLetter, false,
               "UNALLOCATED - the sole free nibble; post-v1 candidates "
               "(JVM IL / .NET IL / shader-stage validators) draw from here"},
    /* 0x4 */ {'R', true,  "register allocator"},
    /* 0x5 */ {'F', true,  "FFI binary-reader + C-header-parser (plan 11 s2.6)"},
    /* 0x6 */ {kUnallocatedFamilyLetter, false,
               "RESERVED for the WAT/WASM verifier (plan 18) - reserved is NOT "
               "allocated: no code may render here until a row claims a letter"},
    /* 0x7 */ {kUnallocatedFamilyLetter, false,
               "RESERVED for the SPIR-V verifier (plan 17) - reserved is NOT "
               "allocated: no code may render here until a row claims a letter"},
    /* 0x8 */ {'K', true,  "linker (plan 14 LK4)"},
    /* 0x9 */ {'P', false, "parser, internal-invariant range (renders P9xxx)"},
    /* 0xA */ {'I', true,  "MIR verifier / IR-gen mid-level"},
    /* 0xB */ {'L', true,  "LIR lowering + verifier"},
    /* 0xC */ {'C', true,  "config"},
    /* 0xD */ {'D', true,  "driver / compilation-unit"},
    /* 0xE */ {'S', true,  "semantic analysis"},
    /* 0xF */ {'H', true,  "HIR verifier / lowering"},
}};

// Structural completeness, at COMPILE TIME. The table is addressed by index, so
// the only way it can be wrong is by being the wrong SHAPE — and that is
// exactly what these catch. A row deleted or added reds the first; a row whose
// `stripsNibble` contradicts the parser's two un-stripped ranges reds the
// second. Neither can be satisfied by a plausible-looking wrong value.
static_assert(kNibbleFamilies.size() == 16,
              "kNibbleFamilies must carry one row per high nibble: the table is "
              "addressed by `v >> 12`, so a missing row is an out-of-bounds "
              "read and an extra row is unreachable");
static_assert(!kNibbleFamilies[0x0].stripsNibble
                  && !kNibbleFamilies[0x9].stripsNibble,
              "the parser's 0x0xxx and 0x9xxx ranges render their FULL value "
              "(P0001, P9000); stripping either would collide them");

} // namespace

char diagnosticFamilyLetter(DiagnosticCode c) noexcept {
    return kNibbleFamilies[static_cast<std::uint16_t>(c) >> 12].letter;
}

bool diagnosticCodeIsAllocated(DiagnosticCode c) noexcept {
    return diagnosticCodeName(c) != kUnallocatedDiagnosticCodeName;
}

std::string diagnosticCodePrefix(DiagnosticCode c) {
    // Render as the 4-digit hex grouping the user actually sees.
    const auto v = static_cast<std::uint16_t>(c);
    NibbleFamily const& fam = kNibbleFamilies[v >> 12];
    // An UNALLOCATED family keeps its whole value (`?3000`, not `?0000`) so the
    // operator can see WHICH nibble was rendered without a real family.
    const std::uint16_t lo = fam.stripsNibble ? (v & 0x0FFFu) : v;
    return std::format("{}{:04X}", fam.letter, lo);
}

} // namespace dss
