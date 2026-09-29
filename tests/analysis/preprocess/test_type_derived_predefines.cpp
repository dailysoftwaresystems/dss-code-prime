// The GNU integer-type predefined-macro families — `__*_TYPE__` (`type-name`),
// `__*_MAX__` / `__*_MIN__` / `__*_WIDTH__` (`type-limit`), `__*_C(c)` and
// `__*_C_SUFFIX__` (`type-suffix`). P68 round 9, lane `lm`.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// ✔MEASURED 2026-09-23 (`--dump-predefined-macros`, every shipped pair at HEAD
// 4d9a24c4): DSS defined NONE of these 122 names. gcc 13.3.0 and mingw-w64 13.2.0
// define the gcc set, clang 18.1.3 the clang set (`-std=c2x -dM -E -x c`), Apple
// clang 21.0.0 its own (the coordinator's Mac). A program that says `__SIZE_TYPE__`
// or `__INT64_C(1)` compiled under every reference and not under DSS.
//
// ★ ONE FACT PER TYPE. GCC documents `__INT_FAST16_TYPE__` as "the correct
// underlying type" of `int_fast16_t`, so a row NAMES the shipped typedef and the
// macro reads the very typedef a program declares: the macro cannot disagree with
// the type. And the typedef itself had to be FIXED: DSS's `stdint.json` said
// `int_fast16_t`/`int_fast32_t` are `short`/`int` on every pair, while glibc says
// `long` for both on x86_64 AND aarch64 (✔MEASURED, gcc and clang, `_Generic` +
// `sizeof`) — the macro would have inherited the wrong type. Darwin (Apple clang
// 21.0.0, both arches) and mingw say `short`/`int`: ✔MEASURED.
//
// ⚖ THE MEANING FORKS, DECIDED (the rows carry the reasoning):
//   * clang 18 on Linux says `__INT_FAST16_TYPE__` `short` while glibc's own
//     <stdint.h> says `long`: the shipped typedef wins (RULED; gcc's answer).
//   * `__BOOL_WIDTH__`: clang 18 `8`, Apple clang 21 `1`, gcc none — `1`
//     (C23 5.2.5.3.2 fn 15, the text that decided `BOOL_WIDTH`).
//
// Pins: (1) the family is exactly the references' union, as LANGUAGE rows naming
// types, and no target or format declares one; (2) on every real pair every name
// has the reference's type / value / width / suffix, AND each `__X_TYPE__` is the
// same type as the typedef it names; (3) the merge realizes them from the shipped
// descriptor for the pair and the language, and from nothing else; (4) the loader
// refuses every malformed row; (5) the merge refuses, loud, a type the pair
// realizes and the language cannot answer for.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// ── THE FAMILY: every name, its kind, and the TYPE KEY its value is a fact of ──
// A key is a shipped typedef name, a C type name, or `void*` (a pointer's width).
// A literal list, not a generated one: it is what a reader audits.
enum class Aspect { Type, Max, Min, Width, CFn, CSuffix };
struct Member {
    std::string_view name;
    Aspect           aspect;
    std::string_view key;
};

constexpr Member kMembers[] = {
    // ── __*_TYPE__ (36) ────────────────────────────────────────────────────
    {"__CHAR8_TYPE__",        Aspect::Type, "unsigned char"},
    {"__CHAR16_TYPE__",       Aspect::Type, "uint_least16_t"},
    {"__CHAR32_TYPE__",       Aspect::Type, "uint_least32_t"},
    {"__INT8_TYPE__",         Aspect::Type, "int8_t"},
    {"__INT16_TYPE__",        Aspect::Type, "int16_t"},
    {"__INT32_TYPE__",        Aspect::Type, "int32_t"},
    {"__INT64_TYPE__",        Aspect::Type, "int64_t"},
    {"__UINT8_TYPE__",        Aspect::Type, "uint8_t"},
    {"__UINT16_TYPE__",       Aspect::Type, "uint16_t"},
    {"__UINT32_TYPE__",       Aspect::Type, "uint32_t"},
    {"__UINT64_TYPE__",       Aspect::Type, "uint64_t"},
    {"__INT_LEAST8_TYPE__",   Aspect::Type, "int_least8_t"},
    {"__INT_LEAST16_TYPE__",  Aspect::Type, "int_least16_t"},
    {"__INT_LEAST32_TYPE__",  Aspect::Type, "int_least32_t"},
    {"__INT_LEAST64_TYPE__",  Aspect::Type, "int_least64_t"},
    {"__UINT_LEAST8_TYPE__",  Aspect::Type, "uint_least8_t"},
    {"__UINT_LEAST16_TYPE__", Aspect::Type, "uint_least16_t"},
    {"__UINT_LEAST32_TYPE__", Aspect::Type, "uint_least32_t"},
    {"__UINT_LEAST64_TYPE__", Aspect::Type, "uint_least64_t"},
    {"__INT_FAST8_TYPE__",    Aspect::Type, "int_fast8_t"},
    {"__INT_FAST16_TYPE__",   Aspect::Type, "int_fast16_t"},
    {"__INT_FAST32_TYPE__",   Aspect::Type, "int_fast32_t"},
    {"__INT_FAST64_TYPE__",   Aspect::Type, "int_fast64_t"},
    {"__UINT_FAST8_TYPE__",   Aspect::Type, "uint_fast8_t"},
    {"__UINT_FAST16_TYPE__",  Aspect::Type, "uint_fast16_t"},
    {"__UINT_FAST32_TYPE__",  Aspect::Type, "uint_fast32_t"},
    {"__UINT_FAST64_TYPE__",  Aspect::Type, "uint_fast64_t"},
    {"__INTPTR_TYPE__",       Aspect::Type, "intptr_t"},
    {"__UINTPTR_TYPE__",      Aspect::Type, "uintptr_t"},
    {"__INTMAX_TYPE__",       Aspect::Type, "intmax_t"},
    {"__UINTMAX_TYPE__",      Aspect::Type, "uintmax_t"},
    {"__PTRDIFF_TYPE__",      Aspect::Type, "ptrdiff_t"},
    {"__SIZE_TYPE__",         Aspect::Type, "size_t"},
    {"__WCHAR_TYPE__",        Aspect::Type, "wchar_t"},
    {"__WINT_TYPE__",         Aspect::Type, "wint_t"},
    {"__SIG_ATOMIC_TYPE__",   Aspect::Type, "sig_atomic_t"},
    // ── __*_MAX__ / __*_MIN__ (41) ─────────────────────────────────────────
    {"__SCHAR_MAX__",         Aspect::Max, "signed char"},
    {"__SHRT_MAX__",          Aspect::Max, "short"},
    {"__INT_MAX__",           Aspect::Max, "int"},
    {"__LONG_MAX__",          Aspect::Max, "long"},
    {"__LONG_LONG_MAX__",     Aspect::Max, "long long"},
    {"__WCHAR_MAX__",         Aspect::Max, "wchar_t"},
    {"__WCHAR_MIN__",         Aspect::Min, "wchar_t"},
    {"__WINT_MAX__",          Aspect::Max, "wint_t"},
    {"__WINT_MIN__",          Aspect::Min, "wint_t"},
    {"__PTRDIFF_MAX__",       Aspect::Max, "ptrdiff_t"},
    {"__SIZE_MAX__",          Aspect::Max, "size_t"},
    {"__INTMAX_MAX__",        Aspect::Max, "intmax_t"},
    {"__UINTMAX_MAX__",       Aspect::Max, "uintmax_t"},
    {"__SIG_ATOMIC_MAX__",    Aspect::Max, "sig_atomic_t"},
    {"__SIG_ATOMIC_MIN__",    Aspect::Min, "sig_atomic_t"},
    {"__INT8_MAX__",          Aspect::Max, "int8_t"},
    {"__INT16_MAX__",         Aspect::Max, "int16_t"},
    {"__INT32_MAX__",         Aspect::Max, "int32_t"},
    {"__INT64_MAX__",         Aspect::Max, "int64_t"},
    {"__UINT8_MAX__",         Aspect::Max, "uint8_t"},
    {"__UINT16_MAX__",        Aspect::Max, "uint16_t"},
    {"__UINT32_MAX__",        Aspect::Max, "uint32_t"},
    {"__UINT64_MAX__",        Aspect::Max, "uint64_t"},
    {"__INT_LEAST8_MAX__",    Aspect::Max, "int_least8_t"},
    {"__INT_LEAST16_MAX__",   Aspect::Max, "int_least16_t"},
    {"__INT_LEAST32_MAX__",   Aspect::Max, "int_least32_t"},
    {"__INT_LEAST64_MAX__",   Aspect::Max, "int_least64_t"},
    {"__UINT_LEAST8_MAX__",   Aspect::Max, "uint_least8_t"},
    {"__UINT_LEAST16_MAX__",  Aspect::Max, "uint_least16_t"},
    {"__UINT_LEAST32_MAX__",  Aspect::Max, "uint_least32_t"},
    {"__UINT_LEAST64_MAX__",  Aspect::Max, "uint_least64_t"},
    {"__INT_FAST8_MAX__",     Aspect::Max, "int_fast8_t"},
    {"__INT_FAST16_MAX__",    Aspect::Max, "int_fast16_t"},
    {"__INT_FAST32_MAX__",    Aspect::Max, "int_fast32_t"},
    {"__INT_FAST64_MAX__",    Aspect::Max, "int_fast64_t"},
    {"__UINT_FAST8_MAX__",    Aspect::Max, "uint_fast8_t"},
    {"__UINT_FAST16_MAX__",   Aspect::Max, "uint_fast16_t"},
    {"__UINT_FAST32_MAX__",   Aspect::Max, "uint_fast32_t"},
    {"__UINT_FAST64_MAX__",   Aspect::Max, "uint_fast64_t"},
    {"__INTPTR_MAX__",        Aspect::Max, "intptr_t"},
    {"__UINTPTR_MAX__",       Aspect::Max, "uintptr_t"},
    // ── __*_WIDTH__ (25) ───────────────────────────────────────────────────
    {"__SCHAR_WIDTH__",       Aspect::Width, "signed char"},
    {"__SHRT_WIDTH__",        Aspect::Width, "short"},
    {"__INT_WIDTH__",         Aspect::Width, "int"},
    {"__LONG_WIDTH__",        Aspect::Width, "long"},
    {"__LONG_LONG_WIDTH__",   Aspect::Width, "long long"},
    {"__LLONG_WIDTH__",       Aspect::Width, "long long"},
    {"__BOOL_WIDTH__",        Aspect::Width, "bool"},
    {"__PTRDIFF_WIDTH__",     Aspect::Width, "ptrdiff_t"},
    {"__SIZE_WIDTH__",        Aspect::Width, "size_t"},
    {"__WCHAR_WIDTH__",       Aspect::Width, "wchar_t"},
    {"__WINT_WIDTH__",        Aspect::Width, "wint_t"},
    {"__SIG_ATOMIC_WIDTH__",  Aspect::Width, "sig_atomic_t"},
    {"__INTMAX_WIDTH__",      Aspect::Width, "intmax_t"},
    {"__UINTMAX_WIDTH__",     Aspect::Width, "uintmax_t"},
    {"__INTPTR_WIDTH__",      Aspect::Width, "intptr_t"},
    {"__UINTPTR_WIDTH__",     Aspect::Width, "uintptr_t"},
    {"__POINTER_WIDTH__",     Aspect::Width, "void*"},
    {"__INT_LEAST8_WIDTH__",  Aspect::Width, "int_least8_t"},
    {"__INT_LEAST16_WIDTH__", Aspect::Width, "int_least16_t"},
    {"__INT_LEAST32_WIDTH__", Aspect::Width, "int_least32_t"},
    {"__INT_LEAST64_WIDTH__", Aspect::Width, "int_least64_t"},
    {"__INT_FAST8_WIDTH__",   Aspect::Width, "int_fast8_t"},
    {"__INT_FAST16_WIDTH__",  Aspect::Width, "int_fast16_t"},
    {"__INT_FAST32_WIDTH__",  Aspect::Width, "int_fast32_t"},
    {"__INT_FAST64_WIDTH__",  Aspect::Width, "int_fast64_t"},
    // ── __*_C(c) and __*_C_SUFFIX__ (20) ───────────────────────────────────
    {"__INT8_C",              Aspect::CFn, "int_least8_t"},
    {"__INT16_C",             Aspect::CFn, "int_least16_t"},
    {"__INT32_C",             Aspect::CFn, "int_least32_t"},
    {"__INT64_C",             Aspect::CFn, "int_least64_t"},
    {"__UINT8_C",             Aspect::CFn, "uint_least8_t"},
    {"__UINT16_C",            Aspect::CFn, "uint_least16_t"},
    {"__UINT32_C",            Aspect::CFn, "uint_least32_t"},
    {"__UINT64_C",            Aspect::CFn, "uint_least64_t"},
    {"__INTMAX_C",            Aspect::CFn, "intmax_t"},
    {"__UINTMAX_C",           Aspect::CFn, "uintmax_t"},
    {"__INT8_C_SUFFIX__",     Aspect::CSuffix, "int_least8_t"},
    {"__INT16_C_SUFFIX__",    Aspect::CSuffix, "int_least16_t"},
    {"__INT32_C_SUFFIX__",    Aspect::CSuffix, "int_least32_t"},
    {"__INT64_C_SUFFIX__",    Aspect::CSuffix, "int_least64_t"},
    {"__UINT8_C_SUFFIX__",    Aspect::CSuffix, "uint_least8_t"},
    {"__UINT16_C_SUFFIX__",   Aspect::CSuffix, "uint_least16_t"},
    {"__UINT32_C_SUFFIX__",   Aspect::CSuffix, "uint_least32_t"},
    {"__UINT64_C_SUFFIX__",   Aspect::CSuffix, "uint_least64_t"},
    {"__INTMAX_C_SUFFIX__",   Aspect::CSuffix, "intmax_t"},
    {"__UINTMAX_C_SUFFIX__",  Aspect::CSuffix, "uintmax_t"},
};

// ── THE REFERENCES' TYPES, PER (PROCESSOR, PLATFORM) ─────────────────────────
// Written out, never read from DSS's own documents: a pin that read the shipped
// descriptors to predict them would agree with any mistake in them. ✔MEASURED
// 2026-09-23 by `_Generic` + `sizeof` (carry `darwin_stdint_types.c`) and by
// `-dM`: glibc with gcc 13.3.0 AND clang 18.1.3 on x86_64 and aarch64 (their cross
// compilers, qemu); mingw-w64 13.2.0 (the pe pair presents mingw's identity);
// Apple clang 21.0.0 on arm64 and x86_64 (the coordinator's Mac). Every typedef the
// table does not list is the same on all five platforms (`kCommon`).
struct Platform {
    std::string_view target;
    ObjectFormatKind format;
    unsigned         longBits;            // the data model's `long`
    // DSS ships <signal.h> here — not on pe: D-C-SIGNAL-H-INCOMPLETE-AND-ABSENT-ON-PE.
    bool             signalShipped;
    std::map<std::string_view, std::string_view> types;   // key → C type
    std::string_view source;
};

std::map<std::string_view, std::string_view> const kCommon{
    {"int8_t", "signed char"},    {"int16_t", "short"},          {"int32_t", "int"},
    {"uint8_t", "unsigned char"}, {"uint16_t", "unsigned short"}, {"uint32_t", "unsigned int"},
    {"int_least8_t", "signed char"},    {"int_least16_t", "short"},
    {"int_least32_t", "int"},           {"uint_least8_t", "unsigned char"},
    {"uint_least16_t", "unsigned short"}, {"uint_least32_t", "unsigned int"},
    {"int_fast8_t", "signed char"},     {"uint_fast8_t", "unsigned char"},
    {"sig_atomic_t", "int"},
    {"signed char", "signed char"}, {"short", "short"}, {"int", "int"},
    {"long", "long"}, {"long long", "long long"}, {"unsigned char", "unsigned char"},
};

std::map<std::string_view, std::string_view> const kGlibc{
    {"int64_t", "long"},  {"uint64_t", "unsigned long"},
    {"int_least64_t", "long"}, {"uint_least64_t", "unsigned long"},
    {"int_fast16_t", "long"},  {"int_fast32_t", "long"},  {"int_fast64_t", "long"},
    {"uint_fast16_t", "unsigned long"}, {"uint_fast32_t", "unsigned long"},
    {"uint_fast64_t", "unsigned long"},
    {"intptr_t", "long"}, {"uintptr_t", "unsigned long"},
    {"intmax_t", "long"}, {"uintmax_t", "unsigned long"},
    {"ptrdiff_t", "long"}, {"size_t", "unsigned long"}, {"wint_t", "unsigned int"},
};

[[nodiscard]] std::map<std::string_view, std::string_view>
merged(std::map<std::string_view, std::string_view> a,
       std::map<std::string_view, std::string_view> const& b) {
    for (auto const& [k, v] : b) a[k] = v;
    for (auto const& [k, v] : kCommon) a.emplace(k, v);
    return a;
}

std::vector<Platform> const& platforms() {
    static std::vector<Platform> const v = [] {
        std::vector<Platform> out;
        auto glibc = [&](std::string_view target, std::string_view wchar) {
            auto t = merged(kGlibc, {{"wchar_t", wchar}});
            return Platform{target, ObjectFormatKind::Elf, 64, true, std::move(t),
                            "gcc 13.3.0 + clang 18.1.3 (glibc), MEASURED"};
        };
        out.push_back(glibc("x86_64", "int"));
        out.push_back(glibc("arm64", "unsigned int"));
        out.push_back(Platform{"x86_64", ObjectFormatKind::Pe, 32, false,
            merged({{"int64_t", "long long"}, {"uint64_t", "unsigned long long"},
                    {"int_least64_t", "long long"}, {"uint_least64_t", "unsigned long long"},
                    {"int_fast16_t", "short"}, {"int_fast32_t", "int"},
                    {"int_fast64_t", "long long"}, {"uint_fast16_t", "unsigned short"},
                    {"uint_fast32_t", "unsigned int"}, {"uint_fast64_t", "unsigned long long"},
                    {"intptr_t", "long long"}, {"uintptr_t", "unsigned long long"},
                    {"intmax_t", "long long"}, {"uintmax_t", "unsigned long long"},
                    {"ptrdiff_t", "long long"}, {"size_t", "unsigned long long"},
                    {"wchar_t", "unsigned short"}, {"wint_t", "unsigned short"}}, {}),
            "mingw-w64 gcc 13.2.0, MEASURED"});
        auto darwin = [&](std::string_view target) {
            return Platform{target, ObjectFormatKind::MachO, 64, true,
                merged({{"int64_t", "long long"}, {"uint64_t", "unsigned long long"},
                        {"int_least64_t", "long long"}, {"uint_least64_t", "unsigned long long"},
                        {"int_fast16_t", "short"}, {"int_fast32_t", "int"},
                        {"int_fast64_t", "long long"}, {"uint_fast16_t", "unsigned short"},
                        {"uint_fast32_t", "unsigned int"}, {"uint_fast64_t", "unsigned long long"},
                        {"intptr_t", "long"}, {"uintptr_t", "unsigned long"},
                        {"intmax_t", "long"}, {"uintmax_t", "unsigned long"},
                        {"ptrdiff_t", "long"}, {"size_t", "unsigned long"},
                        {"wchar_t", "int"}, {"wint_t", "int"}}, {}),
                "Apple clang 21.0.0, MEASURED on the coordinator's Mac"};
        };
        out.push_back(darwin("x86_64"));
        out.push_back(darwin("arm64"));
        return out;
    }();
    return v;
}

[[nodiscard]] Platform const* platformFor(std::string_view target, ObjectFormatKind f) {
    for (auto const& p : platforms()) {
        if (p.target == target && p.format == f) return &p;
    }
    return nullptr;
}

// A C integer type's width and signedness on a platform (C's own facts).
struct IntFacts {
    unsigned bits;
    bool     isSigned;
};
[[nodiscard]] IntFacts factsOf(std::string_view cType, Platform const& p) {
    if (cType == "signed char") return {8, true};
    if (cType == "unsigned char") return {8, false};
    if (cType == "short") return {16, true};
    if (cType == "unsigned short") return {16, false};
    if (cType == "int") return {32, true};
    if (cType == "unsigned int") return {32, false};
    if (cType == "long") return {p.longBits, true};
    if (cType == "unsigned long") return {p.longBits, false};
    if (cType == "long long") return {64, true};
    if (cType == "unsigned long long") return {64, false};
    ADD_FAILURE() << "no integer facts for '" << cType << "'";
    return {0, true};
}
// C 6.3.1.1: a type narrower than `int` promotes to `int`; the others stay.
[[nodiscard]] std::string_view promoted(std::string_view cType, Platform const& p) {
    return factsOf(cType, p).bits < 32 ? std::string_view{"int"} : cType;
}
[[nodiscard]] std::string maxLiteral(IntFacts f) {
    if (f.isSigned) {
        return std::format("{}LL", (std::uint64_t{1} << (f.bits - 1)) - 1);
    }
    std::uint64_t const m = f.bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << f.bits) - 1;
    return std::format("{}ULL", m);
}
[[nodiscard]] std::string minLiteral(IntFacts f) {
    if (!f.isSigned) return "0";
    return std::format("(-{}LL - 1)", (std::uint64_t{1} << (f.bits - 1)) - 1);
}

[[nodiscard]] bool definedOn(Member const& m, Platform const& p) {
    return p.signalShipped || m.key != "sig_atomic_t";
}

// The probe one pair compiles: every member's reference answer, as C that must
// compile — in expressions (`_Generic` identity, `==` value) and in `#if`.
[[nodiscard]] std::string probeFor(Platform const& p) {
    std::string s =
        "#include <stdint.h>\n#include <stddef.h>\n"
        "#define CAT_(a, b) a##b\n#define CAT(a, b) CAT_(a, b)\n"
        "#define IS(T, E) _Static_assert(_Generic((E), T: 1, default: 0), #E \" is not \" #T);\n";
    if (p.signalShipped) s += "#include <signal.h>\n";
    for (Member const& m : kMembers) {
        if (!definedOn(m, p)) {
            s += std::format("#ifdef {0}\n#error \"{0} must be undefined here: DSS ships no "
                             "<signal.h> on this platform\"\n#endif\n", m.name);
            continue;
        }
        s += std::format("#ifndef {0}\n#error \"{0} is not defined\"\n#endif\n", m.name);
        if (m.aspect == Aspect::Width) {
            unsigned const w = m.key == "void*" ? 64u
                             : m.key == "bool"  ? 1u
                                                : factsOf(p.types.at(m.key), p).bits;
            s += std::format("IS(int, {0})\n_Static_assert({0} == {1}, \"{0}\");\n"
                             "#if {0} != {1}\n#error \"{0} in #if\"\n#endif\n", m.name, w);
            continue;
        }
        std::string_view const type = p.types.at(m.key);
        IntFacts const f = factsOf(type, p);
        switch (m.aspect) {
            case Aspect::Type:
                s += std::format("IS({0}, ({1})0)\n", type, m.name);
                // ONE FACT: the macro IS the typedef a program declares, where DSS
                // ships that typedef (no <wchar.h>/<uchar.h>: wint_t, char16/32_t).
                if (m.key != "wint_t" && m.key != "unsigned char") {
                    s += std::format("IS({0}, ({1})0)\n", m.key, m.name);
                }
                break;
            case Aspect::Max:
            case Aspect::Min: {
                std::string const v = m.aspect == Aspect::Max ? maxLiteral(f) : minLiteral(f);
                s += std::format("IS({0}, {1})\n_Static_assert({1} == {2}, \"{1}\");\n"
                                 "#if {1} != {2}\n#error \"{1} in #if\"\n#endif\n",
                                 promoted(type, p), m.name, v);
                break;
            }
            case Aspect::CFn:
                s += std::format("IS({0}, {1}(1))\n_Static_assert({1}(1) == 1, \"{1}\");\n",
                                 promoted(type, p), m.name);
                break;
            case Aspect::CSuffix:
                s += std::format("IS({0}, CAT(1, {1}))\n", promoted(type, p), m.name);
                break;
            case Aspect::Width:
                break;
        }
    }
    return s + "int dss_type_derived_probe;\n";
}

[[nodiscard]] std::vector<std::string> shippedNames(std::string_view dir,
                                                    std::string_view suffix) {
    std::vector<std::string> out;
    for (auto const& e :
         std::filesystem::directory_iterator{dss::test::configRoot() / dir}) {
        std::string const fn = e.path().filename().string();
        auto const at = fn.find(suffix);
        if (at == std::string::npos) continue;
        out.push_back(fn.substr(0, at));
    }
    return out;
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

[[nodiscard]] std::string errorsOf(TargetSchema const& target, ObjectFormatSchema const& format,
                                   std::string source) {
    UnitBuilder builder{cLanguage(), DiagnosticBudget::libraryDefault()};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "probe.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());
    std::string out;
    auto const collect = [&](DiagnosticReporter const& rep) {
        for (auto const& d : rep.all()) {
            if (d.severity == DiagnosticSeverity::Error) out += "  " + d.actual + "\n";
        }
    };
    collect(cu->driverDiagnostics());
    for (auto const& tree : cu->trees()) collect(tree.diagnostics());
    auto const analysis = analyzeForTargetFormat(cu, DiagnosticBudget::libraryDefault(),
                                                 target, format, nullptr);
    collect(analysis.model.diagnostics());
    return out;
}

[[nodiscard]] bool isFamilyKind(PredefinedMacroKind k) {
    return k == PredefinedMacroKind::TypeName || k == PredefinedMacroKind::TypeLimit
        || k == PredefinedMacroKind::TypeSuffix;
}

}  // namespace

// ── (1) THE FAMILY IS THE REFERENCES' UNION, AS LANGUAGE ROWS NAMING TYPES ────
TEST(TypeDerivedPredefines, TheFamilyIsTheReferencesUnionAsLanguageRowsNamingTypes) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const& rows = cLanguage()->preprocess().predefinedMacros;
    std::size_t familyRows = 0;
    for (auto const& pm : rows) familyRows += isFamilyKind(pm.kind) ? 1u : 0u;
    EXPECT_EQ(familyRows, std::size(kMembers))
        << "the language declares a type-derived row this pin does not know, or lost one";
    for (Member const& m : kMembers) {
        auto const it = std::ranges::find(rows, m.name, &PredefinedMacroDef::name);
        ASSERT_NE(it, rows.end()) << m.name << " is not a row of the C language";
        PredefinedMacroKind const want =
            m.aspect == Aspect::Type ? PredefinedMacroKind::TypeName
            : (m.aspect == Aspect::CFn || m.aspect == Aspect::CSuffix)
                ? PredefinedMacroKind::TypeSuffix
                : PredefinedMacroKind::TypeLimit;
        EXPECT_EQ(it->kind, want) << m.name;
        EXPECT_TRUE(it->value.empty()) << m.name << ": a type-derived row states no value";
        EXPECT_NE(it->sizedType.source, PredefinedTypeSource::None) << m.name;
        EXPECT_TRUE(it->availableObjectFormats.empty())
            << m.name << ": the TYPE decides where it is defined, never a format list";
        EXPECT_EQ(it->isFunctionLike, m.aspect == Aspect::CFn) << m.name;
        if (m.aspect == Aspect::Max) EXPECT_EQ(it->typeLimit, IntegerTypeLimit::Max) << m.name;
        if (m.aspect == Aspect::Min) EXPECT_EQ(it->typeLimit, IntegerTypeLimit::Min) << m.name;
        if (m.aspect == Aspect::Width) EXPECT_EQ(it->typeLimit, IntegerTypeLimit::Width) << m.name;
        // A shipped typedef names ITSELF as the key: the one-fact rule.
        if (it->sizedType.source == PredefinedTypeSource::ShippedTypedef) {
            EXPECT_EQ(it->sizedType.spelled, m.key) << m.name;
        }
    }
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto t = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(t.has_value()) << targetName;
        for (auto const& pm : (*t)->predefinedMacros()) {
            for (Member const& m : kMembers) {
                EXPECT_NE(pm.name, m.name) << targetName << " declares " << pm.name
                                           << " — a second owner";
            }
        }
    }
    for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
        auto f = ObjectFormatSchema::loadShipped(formatName);
        ASSERT_TRUE(f.has_value()) << formatName;
        for (auto const& pm : (*f)->predefinedMacros()) {
            for (Member const& m : kMembers) {
                EXPECT_NE(pm.name, m.name) << formatName << " declares " << pm.name
                                           << " — a second owner";
            }
        }
    }
}

// ── (2) EVERY REAL PAIR: THE REFERENCES' ANSWERS, AND ONE FACT PER TYPE ───────
TEST(TypeDerivedPredefines, EveryNameHasThePlatformsReferenceAnswerOnEveryRealPair) {
    ASSERT_NE(cLanguage(), nullptr);
    std::size_t pairs = 0;
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;   // not a real pair
            SCOPED_TRACE(targetName + ":" + formatName);
            Platform const* p = platformFor((*targetR)->name(), (*formatR)->kind());
            if (p == nullptr) {
                ADD_FAILURE() << "no reference measurement for this pair — measure it";
                continue;
            }
            ++pairs;
            EXPECT_EQ(errorsOf(**targetR, **formatR, probeFor(*p)), "") << "(" << p->source << ")";
        }
    }
    // FLOOR: ✔MEASURED at P68 round 9 — 22 real pairs (13 x86_64 + 9 arm64 formats).
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
}

// ── (3) THE MERGE: THE PAIR'S SHIPPED DESCRIPTOR AND THE LANGUAGE, NOTHING ELSE ──
namespace {

[[nodiscard]] std::map<std::string, std::string>
realized(PredefinedTypeFacts const* facts, GrammarSchema const* language,
         ObjectFormatKind format, std::vector<std::string>* conflicts = nullptr) {
    auto const& rows = (language != nullptr ? *language : *cLanguage()).preprocess().predefinedMacros;
    auto const m = mergePredefinedMacros(rows, {}, {}, format, {}, facts, language);
    if (conflicts != nullptr) *conflicts = m.conflicts;
    else EXPECT_TRUE(m.conflicts.empty()) << (m.conflicts.empty() ? "" : m.conflicts.front());
    std::map<std::string, std::string> out;
    for (auto const& pm : m.effective) {
        if (isFamilyKind(pm.kind)) out[pm.name] = pm.value;
    }
    return out;
}

[[nodiscard]] PredefinedTypeFacts elfFacts() {
    PredefinedTypeFacts f;
    f.dataModel      = DataModel::Lp64;
    f.abiTypedefs    = {{"wchar_t", TypeKind::I32}, {"wint_t", TypeKind::U32}};
    f.charIsUnsigned = false;
    f.targetName     = "x86_64";
    return f;
}

}  // namespace

TEST(TypeDerivedPredefines, TheMergeRealizesEachFromThePairsDescriptorAndTheLanguage) {
    ASSERT_NE(cLanguage(), nullptr);
    PredefinedTypeFacts const elf = elfFacts();
    auto const onElf = realized(&elf, cLanguage().get(), ObjectFormatKind::Elf);
    EXPECT_EQ(onElf.size(), std::size(kMembers)) << "every name realizes on x86_64 ELF";
    EXPECT_EQ(onElf.at("__INT_FAST16_TYPE__"), "long") << "glibc's int_fast16_t";
    EXPECT_EQ(onElf.at("__SIZE_TYPE__"), "unsigned long");
    EXPECT_EQ(onElf.at("__WINT_TYPE__"), "unsigned int") << "the target's ABI typedef";
    EXPECT_EQ(onElf.at("__BOOL_WIDTH__"), "1");
    EXPECT_EQ(onElf.at("__POINTER_WIDTH__"), "64");
    EXPECT_EQ(onElf.at("__INT8_C_SUFFIX__"), "") << "int_least8_t promotes to int";

    // The SAME rows on the pe facts: the descriptor's pe variants, LLP64 — and no
    // <signal.h>, so no `__SIG_ATOMIC_*`.
    PredefinedTypeFacts pe = elf;
    pe.dataModel   = DataModel::Llp64;
    pe.abiTypedefs = {{"wchar_t", TypeKind::U16}, {"wint_t", TypeKind::U16}};
    auto const onPe = realized(&pe, cLanguage().get(), ObjectFormatKind::Pe);
    EXPECT_EQ(onPe.at("__INT_FAST16_TYPE__"), "short") << "mingw's int_fast16_t";
    EXPECT_EQ(onPe.at("__INT64_TYPE__"), "long long");
    EXPECT_EQ(onPe.at("__LONG_WIDTH__"), "32");
    EXPECT_EQ(onPe.at("__WCHAR_TYPE__"), "unsigned short");
    EXPECT_FALSE(onPe.contains("__SIG_ATOMIC_TYPE__")) << "no <signal.h> ships on pe";
    EXPECT_EQ(onPe.size(), std::size(kMembers) - 4);

    // No language, or no pair: none of them — never a guess.
    EXPECT_TRUE(realized(&elf, nullptr, ObjectFormatKind::Elf).empty());
    EXPECT_TRUE(realized(nullptr, cLanguage().get(), ObjectFormatKind::Elf).empty());
}

// ── (4) THE LOADER ──────────────────────────────────────────────────────────
namespace {

[[nodiscard]] std::string shippedCText() {
    std::ifstream in(dss::test::configRoot() / "sources" / "c.lang.json", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string shippedCEdited(std::string_view anchor, std::string_view from,
                                         std::string_view to) {
    std::string text = shippedCText();
    auto const at = text.find(anchor);
    if (at == std::string::npos) {
        ADD_FAILURE() << "the shipped c config no longer carries " << anchor;
        return {};
    }
    auto const pos = text.find(from, at);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "no '" << from << "' after " << anchor;
        return {};
    }
    text.replace(pos, from.size(), to);
    return text;
}

void refusedNaming(std::string const& text, std::string_view needle) {
    if (text.empty()) return;
    auto r = GrammarSchema::loadFromText(text, "<edited-c>");
    if (r.has_value()) {
        ADD_FAILURE() << "the edited document LOADED — it had to be refused";
        return;
    }
    bool named = false;
    for (auto const& d : r.error()) {
        named |= d.message.find(needle) != std::string::npos
                 || d.path.find(needle) != std::string::npos;
    }
    EXPECT_TRUE(named) << "no diagnostic names '" << needle << "'; first: "
                       << (r.error().empty() ? "<none>" : r.error()[0].message);
}

constexpr std::string_view kFast16Type = "\"__INT_FAST16_TYPE__\"";
constexpr std::string_view kFast16Max  = "\"__INT_FAST16_MAX__\"";
constexpr std::string_view kInt64C     = "\"__INT64_C\"";
constexpr std::string_view kShipped16  =
    "{ \"shippedTypedef\": \"int_fast16_t\", \"header\": \"stdint.h\" }";

}  // namespace

TEST(TypeDerivedPredefinesLoader, TheShippedDocumentLoads) {
    auto r = GrammarSchema::loadFromText(shippedCText(), "<shipped-c>");
    ASSERT_TRUE(r.has_value()) << (r.error().empty() ? "<none>" : r.error()[0].message);
}

TEST(TypeDerivedPredefinesLoader, ALimitRowWithoutItsLimitIsRefused) {
    refusedNaming(shippedCEdited(kFast16Max, ", \"limit\": \"max\"", ""), "requires 'limit'");
}

TEST(TypeDerivedPredefinesLoader, AnUnknownLimitIsRefusedWithTheAcceptedSet) {
    refusedNaming(shippedCEdited(kFast16Max, "\"limit\": \"max\"", "\"limit\": \"maximum\""),
                  "'max' / 'min' / 'width'");
}

TEST(TypeDerivedPredefinesLoader, AShippedTypedefWithoutItsHeaderIsRefused) {
    refusedNaming(shippedCEdited(kFast16Type, ", \"header\": \"stdint.h\"", ""),
                  "requires 'header'");
}

TEST(TypeDerivedPredefinesLoader, AHeaderBesideAnotherTypeSourceIsRefused) {
    refusedNaming(shippedCEdited(kFast16Type, kShipped16,
                                 "{ \"abiTypedef\": \"wint_t\", \"header\": \"stdint.h\" }"),
                  "companion");
}

TEST(TypeDerivedPredefinesLoader, ASizeRowCannotNameAShippedTypedef) {
    refusedNaming(shippedCEdited("\"__SIZEOF_WINT_T__\"", "{ \"abiTypedef\": \"wint_t\" }",
                                 kShipped16),
                  "core alone");
}

TEST(TypeDerivedPredefinesLoader, APointerHasOnlyAWidth) {
    refusedNaming(shippedCEdited(kFast16Type, kShipped16, "{ \"pointerTo\": \"void\" }"),
                  "a pointer");
    refusedNaming(shippedCEdited(kFast16Max, kShipped16, "{ \"pointerTo\": \"void\" }"),
                  "a pointer");
}

TEST(TypeDerivedPredefinesLoader, ParamsBelongToAConstantOrASuffixRow) {
    refusedNaming(shippedCEdited(kFast16Max, "\"limit\": \"max\"",
                                 "\"limit\": \"max\", \"params\": [\"c\"]"),
                  "'params' is valid only");
    refusedNaming(shippedCEdited(kInt64C, "\"params\": [\"c\"]", "\"params\": [\"c\", \"d\"]"),
                  "exactly ONE parameter");
}

TEST(TypeDerivedPredefinesLoader, AValueBesideTheTypeIsRefused) {
    refusedNaming(shippedCEdited(kFast16Type, kShipped16,
                                 std::string{kShipped16} + ", \"value\": \"long\""),
                  "Remove 'value'");
}

TEST(TypeDerivedPredefinesLoader, TwoSpellingsOfOneTypeAreRefused) {
    refusedNaming(shippedCEdited("\"typeNameSpellings\"", "\"short\", ", "\"short\", \"short int\", "),
                  "name ONE type");
}

TEST(TypeDerivedPredefinesLoader, ASpellingThatIsNotAnIntegerTypeIsRefused) {
    refusedNaming(shippedCEdited("\"typeNameSpellings\"", "\"int\", ", "\"int\", \"double\", "),
                  "not an integer type");
}

// `__CHAR8_TYPE__` names the vocabulary type `unsigned char`: with that name gone
// from the list, the row is refused at LOAD, not on the first pair that meets it.
TEST(TypeDerivedPredefinesLoader, AVocabularyTypeTheListCannotSpellIsRefusedAtLoad) {
    refusedNaming(shippedCEdited("\"typeNameSpellings\"", "\"unsigned char\", ", ""),
                  "lists no name for");
}

TEST(TypeDerivedPredefinesLoader, ATargetDocumentCannotDeclareTheKinds) {
    // The kinds as the ONE table spells them — never retyped here.
    std::vector<std::string_view> kinds;
    for (auto const& [k, spelling] : kPredefinedMacroKindTable.rows) {
        if (predefinedMacroKindNeedsLanguage(k)) kinds.push_back(spelling);
    }
    ASSERT_EQ(kinds.size(), 3u);
    for (std::string_view kind : kinds) {
        // A row that is well-formed for its kind in every other respect — `limit`
        // on the `type-limit` row alone — so the family is the ONE thing refused.
        bool const isLimit =
            kind == predefinedMacroKindName(PredefinedMacroKind::TypeLimit);
        auto r = TargetSchema::loadFromText(
            std::format(R"({{"dssTargetVersion":1,"target":{{"name":"X"}},
                "opcodes":[{{"mnemonic":"invalid","result":"none"}}],
                "predefinedMacros":[{{"name":"__X__","kind":"{}","type":"int",{}
                  "programRedefinition":"ordinary",
                  "impliedSurface":{{"kind":"claims-nothing","reason":"arch-property"}}}}]}})",
                        kind, isLimit ? R"("limit":"max",)" : ""),
            "<inline>");
        ASSERT_FALSE(r.has_value()) << "a target document must not declare a '" << kind << "' row";
        bool named = false;
        for (auto const& d : r.error()) {
            named |= d.message.find("LANGUAGE-family") != std::string::npos;
        }
        EXPECT_TRUE(named) << kind << ": the refusal must say the kind is the language's";
    }
}

// ── (5) THE MERGE REFUSES, LOUD, WHAT THE PAIR REALIZES AND THE LANGUAGE CANNOT ──
TEST(TypeDerivedPredefines, ATypeThePairRealizesButTheLanguageCannotSpellIsAConflict) {
    // `long` dropped from the list: nothing at LOAD names it through a vocabulary
    // row, so the document loads — and the ELF pair's `int64_t` (`long`) then has
    // no spelling. The merge must refuse, naming the macro, never guess one.
    std::string const text = shippedCEdited("\"typeNameSpellings\"", "\"long\", ", "");
    auto edited = GrammarSchema::loadFromText(text, "<edited-c>");
    ASSERT_TRUE(edited.has_value()) << (edited.error().empty() ? "" : edited.error()[0].message);
    PredefinedTypeFacts const elf = elfFacts();
    std::vector<std::string> conflicts;
    auto const out = realized(&elf, edited->get(), ObjectFormatKind::Elf, &conflicts);
    bool named = false;
    for (auto const& c : conflicts) {
        named |= c.find("__INT64_TYPE__") != std::string::npos
                 && c.find("lists no name for") != std::string::npos;
    }
    EXPECT_TRUE(named) << "the conflict must name __INT64_TYPE__ and why";
    EXPECT_TRUE(out.empty()) << "a merge with conflicts yields no effective list";
}

TEST(TypeDerivedPredefines, AShippedTypedefOfAHeaderDssDoesNotShipIsAConflict) {
    std::string const text =
        shippedCEdited(kFast16Type, "\"header\": \"stdint.h\"", "\"header\": \"no_such_header.h\"");
    auto edited = GrammarSchema::loadFromText(text, "<edited-c>");
    ASSERT_TRUE(edited.has_value()) << (edited.error().empty() ? "" : edited.error()[0].message);
    PredefinedTypeFacts const elf = elfFacts();
    std::vector<std::string> conflicts;
    (void)realized(&elf, edited->get(), ObjectFormatKind::Elf, &conflicts);
    bool named = false;
    for (auto const& c : conflicts) {
        named |= c.find("no_such_header.h") != std::string::npos;
    }
    EXPECT_TRUE(named) << "the conflict must name the header";
}

// A typo in the TYPEDEF name must not pass for an absence either: `<stdint.h>`
// ships, but declares no `int_fast16` on any pair, so the row is a conflict — not a
// macro silently undefined on every pair (P68 round 9: `ffi::readShippedHeaderTypedefs`
// returns the header's declared names beside the pair's selected typedefs).
TEST(TypeDerivedPredefines, AShippedTypedefItsHeaderDeclaresOnNoPairIsAConflict) {
    std::string const text = shippedCEdited(kFast16Type, "\"shippedTypedef\": \"int_fast16_t\"",
                                            "\"shippedTypedef\": \"int_fast16\"");
    auto edited = GrammarSchema::loadFromText(text, "<edited-c>");
    ASSERT_TRUE(edited.has_value()) << (edited.error().empty() ? "" : edited.error()[0].message);
    PredefinedTypeFacts const elf = elfFacts();
    std::vector<std::string> conflicts;
    (void)realized(&elf, edited->get(), ObjectFormatKind::Elf, &conflicts);
    bool named = false;
    for (auto const& c : conflicts) {
        named |= c.find("'int_fast16'") != std::string::npos
                 && c.find("__INT_FAST16_TYPE__") != std::string::npos
                 && c.find("declares on no pair") != std::string::npos;
    }
    EXPECT_TRUE(named) << "the conflict must name the macro, the typedef and the reason";
}
