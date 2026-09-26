#pragma once

// ── The constant forms a language admits BEYOND the standard's own list ─────────────────────
//
// C 6.6p7-p9 (C23 6.6p9-p12) list what a constant expression in an initializer may be: an
// arithmetic constant expression, a null pointer constant, an ADDRESS CONSTANT (`&` of an
// object of static storage duration or a function designator, reached through `[]`, `.`,
// `->`, `*` and pointer casts; an array or function designator; an integer constant cast to
// a pointer), or an address constant for a complete object type plus or minus an integer
// constant expression. 6.6p10 (C23 6.6p14) lets an implementation "accept other forms of
// constant expressions", and the reference toolchains do — each differently. A language
// names the ones it admits in `semantics.staticInitializers.otherConstantForms` (its
// `.lang.json`); the ONE list is read by the semantic tier's static-initializer check and —
// `languageMirLoweringConfig` threading it into `MirLoweringConfig::globalsConstantForms` —
// by the constant evaluator the static-data producer asks, so the two can never disagree
// about what a form is.
//
// Every form is a measured one (lane `cs`'s `.temp/probe/sti` … `sti9`, P68 round 12: gcc
// 13.3.0 and clang 18.1.3 at `-std=c17 -pedantic-errors` and at `-std=c2x`, mingw-w64 13.2.0
// in both modes, MSVC 19.51 at `/std:c17` and `/std:clatest`, each separately, every build
// RUN):
//   * ConstObjectRead       — the value of a const-qualified, non-volatile object whose
//                             constant initializer is visible at the read, its elements and
//                             members included (`y + 2` for `static const int y = 40;`,
//                             `c[1]`, `s.m`, a const pointer's address). gcc, clang, mingw-w64.
//                             Volatile is the OBJECT's type looked through its array spine
//                             (`TypeInterner::isVolatileObjectType`): all four refuse `cva[1]`
//                             of `const volatile int cva[2]`, and gcc and mingw-w64 build `s.v`
//                             for a volatile MEMBER of a const, non-volatile `s` (fold F7).
//   * CommaOperator         — a constant that DISCARDS constant operands before its value:
//                             `(e1, e2)` (clang, MSVC), and a GNU statement expression with more
//                             than its value, `({ 1; 42; })` (clang at -std=c2x; the two share one
//                             HIR shape). 6.6p3 names the comma, so the semantic tier admits it
//                             WITH a warning. `({ 42; })` discards nothing and needs no form.
//   * AddressAsInteger      — an address constant converted to an integer type EXACTLY as wide
//                             as a pointer, plus or minus an integer constant, and back to a
//                             pointer (`(unsigned long long)&a + 5`, `(uintptr_t)&a`). All four;
//                             a SYMBOL's address in a narrower integer is refused by all four,
//                             while a NULL-based one is a plain integer constant all four build
//                             (`(int)&((struct T *)0)->m`) and needs no form.
//   * AddressTruthValue     — an address constant's truth value: `!`, `&&`, `||`, a `?:`
//                             condition, a conversion to `_Bool`, a comparison with a null
//                             pointer. gcc, clang, mingw-w64 — never for a weak declaration
//                             without a definition, whose address may be null.
//   * AddressComparison     — `==` / `!=` of two address constants (the same object: by
//                             offset; distinct objects: unequal, even one past the end — gcc's
//                             fold), and `<` `<=` `>` `>=` within one object. gcc, clang,
//                             mingw-w64 (distinct string literals and one-past-the-end: gcc).
//   * AddressDifference     — the difference of two address constants into the same object,
//                             in elements. gcc, clang, mingw-w64.
//   * AddressIntegerAlgebra — the identity and absorbing elements of integer arithmetic on an
//                             address converted to an integer (`* 0`, `& 0`, `% 1`, `* 1`,
//                             `/ 1`, `| 0`, `^ 0`, `<< 0`, `>> 0`, `& ~0`), comparisons of two
//                             such integers into one object or with 0, and their difference.
//                             gcc and mingw-w64.

#include "core/types/enum_name_table.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace dss {

enum class ConstantForm : std::uint8_t {
    ConstObjectRead       = 0,
    CommaOperator         = 1,
    AddressAsInteger      = 2,
    AddressTruthValue     = 3,
    AddressComparison     = 4,
    AddressDifference     = 5,
    AddressIntegerAlgebra = 6,
};
inline constexpr EnumNameTable<ConstantForm, 7> kConstantFormTable{{{
    { ConstantForm::ConstObjectRead,       "constObjectRead"       },
    { ConstantForm::CommaOperator,         "commaOperator"         },
    { ConstantForm::AddressAsInteger,      "addressAsInteger"      },
    { ConstantForm::AddressTruthValue,     "addressTruthValue"     },
    { ConstantForm::AddressComparison,     "addressComparison"     },
    { ConstantForm::AddressDifference,     "addressDifference"     },
    { ConstantForm::AddressIntegerAlgebra, "addressIntegerAlgebra" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kConstantFormTable);

[[nodiscard]] constexpr std::string_view constantFormName(ConstantForm f) noexcept {
    return kConstantFormTable.name(f);
}
[[nodiscard]] constexpr std::optional<ConstantForm>
constantFormFromName(std::string_view s) noexcept {
    return kConstantFormTable.fromName(s);
}

// A set of admitted forms — one bit per `ConstantForm`. EMPTY ⇒ the standard's forms only.
class ConstantForms {
public:
    constexpr void admit(ConstantForm f) noexcept { bits_ |= bit(f); }
    [[nodiscard]] constexpr bool admits(ConstantForm f) const noexcept {
        return (bits_ & bit(f)) != 0;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
    friend constexpr bool operator==(ConstantForms, ConstantForms) noexcept = default;

private:
    [[nodiscard]] static constexpr std::uint32_t bit(ConstantForm f) noexcept {
        return std::uint32_t{1} << static_cast<std::uint32_t>(f);
    }
    std::uint32_t bits_ = 0;
};

} // namespace dss
