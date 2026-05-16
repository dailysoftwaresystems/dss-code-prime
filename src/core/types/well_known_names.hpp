#pragma once

#include <string_view>

// Standard rule and token-kind names recognized by built-in views and
// well-known engine code. Defining them here once gives every caller a
// single source of truth — engine code, semantic passes, and views all
// reference the same string_view, so a typo can't silently mismatch.
//
// A grammar config is free to use any subset of these (or none); the
// associated view's `from(tree, id)` simply returns std::nullopt if the
// rule / token kind isn't present in the tree's interner.

namespace dss::rules {

inline constexpr std::string_view kIdentifier   = "identifier";
inline constexpr std::string_view kLiteral      = "literal";
inline constexpr std::string_view kBinaryExpr   = "binaryExpr";
inline constexpr std::string_view kBlock        = "block";
inline constexpr std::string_view kFunctionDecl = "functionDecl";
inline constexpr std::string_view kVarDecl      = "varDecl";
inline constexpr std::string_view kExprStmt     = "exprStmt";

} // namespace dss::rules

// A subset of the built-in token-kind list pre-registered by every schema
// — the six literal kinds plus Identifier. Eof/Error are excluded because
// they aren't user-viewable. Any grammar config gets these names in its
// token interner whether or not the user redeclared them.
namespace dss::tokens {

inline constexpr std::string_view kIdentifier    = "Identifier";
inline constexpr std::string_view kIntLiteral    = "IntLiteral";
inline constexpr std::string_view kFloatLiteral  = "FloatLiteral";
inline constexpr std::string_view kStringLiteral = "StringLiteral";
inline constexpr std::string_view kCharLiteral   = "CharLiteral";
inline constexpr std::string_view kBoolLiteral   = "BoolLiteral";
inline constexpr std::string_view kNullLiteral   = "NullLiteral";

} // namespace dss::tokens
