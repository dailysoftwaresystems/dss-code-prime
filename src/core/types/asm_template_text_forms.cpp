#include "core/types/asm_template_text_forms.hpp"

#include <format>
#include <string>

namespace dss {

namespace {

[[nodiscard]] bool startsAt(std::string_view text, std::size_t i,
                            std::string_view lexeme) noexcept {
    return !lexeme.empty() && i <= text.size()
        && text.substr(i).starts_with(lexeme);
}

[[nodiscard]] bool isAsciiDigit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] bool isAsciiAlpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Every form the dialect and the language declare, quoted — the inventory a
// refusal hands the author.
[[nodiscard]] std::string declaredInventory(AsmTemplateTextForms const& forms) {
    std::string out;
    auto add = [&](std::string_view f) {
        if (f.empty()) return;
        if (!out.empty()) out += ", ";
        out += '`';
        out += f;
        out += '`';
    };
    add(forms.instanceNumber);
    for (auto const& f : forms.fixed) add(f.form);
    for (auto const& f : forms.featureSelected) add(f.form);
    return out.empty() ? std::string{"none"} : out;
}

}  // namespace

std::expected<std::string, std::string>
expandAsmTemplateTextForms(std::string_view text, AsmTemplateTextForms const& forms,
                           AsmTemplateTextFormContext const& ctx) {
    std::string out;
    out.reserve(text.size());

    AsmTemplateAlternatives const& alt = forms.alternatives;
    bool          inGroup    = false;
    std::uint32_t altIndex   = 0;
    std::size_t   groupStart = 0;
    // Inside a `{…|…}` group only the SELECTED alternative is text; the others
    // are read (so a form inside them is still checked) and dropped.
    auto emit = [&](std::string_view s) {
        if (!inGroup || altIndex == alt.select) out.append(s);
    };

    std::size_t i = 0;
    while (i < text.size()) {
        // ★ A DROPPED ALTERNATIVE IS SKIPPED, NOT READ — gcc's rule (its
        // `do_assembler_dialects`: a placeholder and the byte after it pass as one
        // unit). ✔MEASURED 2026-09-23 (gcc 13.3.0 and clang 18.1.3, x86_64, -S,
        // each inside `.ascii "X<form>Y"`): `{a|%&}`, `{a|%@}` and `{a|%~}` give
        // `a` under gcc and are refused by clang ("invalid % escape in inline
        // assembly string"), so the union accepts and nothing in a dropped
        // alternative is interpreted; `{a|b%}c}` gives `a` under BOTH, so the
        // sigil also keeps the byte after it from closing the group.
        if (inGroup && altIndex != alt.select
            && startsAt(text, i, ctx.placeholder)) {
            i += ctx.placeholder.size();
            if (i < text.size()) ++i;
            continue;
        }
        if (startsAt(text, i, ctx.placeholder)) {
            // The escape first: it EXTENDS the placeholder (the loader guarantees
            // it is the longer lexeme), and it is a literal the lexer reads.
            if (startsAt(text, i, ctx.escape)) {
                emit(ctx.escape);
                i += ctx.escape.size();
                continue;
            }
            if (startsAt(text, i, forms.instanceNumber)) {
                emit(std::to_string(ctx.instance));
                i += forms.instanceNumber.size();
                continue;
            }
            // A declared form, longest first — two forms may share a prefix.
            AsmTemplateFixedForm const*   fixed   = nullptr;
            AsmTemplateFeatureForm const* feature = nullptr;
            std::size_t                   bestLen = 0;
            for (auto const& f : forms.fixed) {
                if (f.form.size() > bestLen && startsAt(text, i, f.form)) {
                    fixed = &f; feature = nullptr; bestLen = f.form.size();
                }
            }
            for (auto const& f : forms.featureSelected) {
                if (f.form.size() > bestLen && startsAt(text, i, f.form)) {
                    feature = &f; fixed = nullptr; bestLen = f.form.size();
                }
            }
            if (fixed != nullptr) {
                emit(fixed->text);
                i += bestLen;
                continue;
            }
            if (feature != nullptr) {
                std::optional<bool> const has =
                    ctx.targetHasFeature ? ctx.targetHasFeature(feature->feature)
                                         : std::nullopt;
                if (!has.has_value()) {
                    return std::unexpected(std::format(
                        "the template form `{}` at byte offset {} expands by whether "
                        "the target has the ISA feature '{}', and this target "
                        "declares no such feature — read as absent, the form would "
                        "silently name the wrong instruction on a target that has it",
                        feature->form, i, feature->feature));
                }
                emit(*has ? feature->present : feature->absent);
                i += bestLen;
                continue;
            }
            std::size_t const after = i + ctx.placeholder.size();
            // A trailing bare placeholder, an operand reference (`%0`,
            // `%[name]`), a modifier or label form (`%w0`, `%l0`): the dialect's
            // LEXER reads these — this pass only steps over the sigil.
            if (after >= text.size() || isAsciiDigit(text[after])
                || isAsciiAlpha(text[after])
                || startsAt(text, after, ctx.symbolicNameOpen)) {
                emit(ctx.placeholder);
                i = after;
                continue;
            }
            return std::unexpected(std::format(
                "the template contains `{}{}` at byte offset {}, which is neither an "
                "operand reference nor a template form this dialect expands (the "
                "forms it declares: {})",
                ctx.placeholder, text[after], i, declaredInventory(forms)));
        }
        if (alt.declared) {
            if (startsAt(text, i, alt.open)) {
                if (inGroup) {
                    return std::unexpected(std::format(
                        "the template opens a dialect alternative `{}` at byte offset "
                        "{} inside the one opened at byte offset {} — nested "
                        "alternatives are refused by both references (gcc: \"nested "
                        "assembly dialect alternatives\"; clang: \"Nested variants "
                        "found in inline asm string\")",
                        alt.open, i, groupStart));
                }
                inGroup    = true;
                altIndex   = 0;
                groupStart = i;
                i += alt.open.size();
                continue;
            }
            if (inGroup && startsAt(text, i, alt.separator)) {
                ++altIndex;
                i += alt.separator.size();
                continue;
            }
            if (inGroup && startsAt(text, i, alt.close)) {
                inGroup = false;
                i += alt.close.size();
                continue;
            }
        }
        emit(text.substr(i, 1));
        ++i;
    }
    if (inGroup) {
        return std::unexpected(std::format(
            "the template opens a dialect alternative `{}` at byte offset {} and "
            "never closes it with `{}`",
            alt.open, groupStart, alt.close));
    }
    return out;
}

} // namespace dss
