// A NUMBER A `.dsshir` SPELLS IS READ AS THAT NUMBER, OR REFUSED NAMING ITS FIELD
// (P68 round 8, lane `ht`, part 1c).
//
// ★★★ WHAT WAS WRONG. Every 32-bit field the reader fills was a bare
// `static_cast<std::uint32_t>(takeInt())`, so `%4294967297` read back as `%1`,
// `buf 4294967297` as buffer 1, `goto L4294967297` as `goto L1` — a DIFFERENT
// symbol, buffer, label — with a clean reporter. Every signed field wrapped the
// same way in the other direction (`arr<i32, 18446744073709551614>` read back as
// the VLA sentinel -2). The symbol table was sized by the LARGEST SLOT the text
// named rather than by the entries it holds: a 40-byte text asked for gigabytes.
// And a handful of fields read a number as a different MEANING: `wfloat 64` as an
// 80-bit float, `bitint` signedness 2 as "signed", the writer's unset shader
// location as "no location", an alignment no layout can honour as a valid one.
//
// THE PINS, one per thing that was wrong:
//   1. THE NARROWING TABLE — one row per field the reader fills, each spelled one
//      past its range (2^32 + 1; 2^63 and -(2^63) - 1 for the signed fields),
//      each a refusal NAMING ITS FIELD and saying what the number would have
//      become. A field that legitimately takes the top of its range carries a
//      CONTROL there, which must read back as spelled — so the check is at the
//      boundary, not below it. A field whose top value is not a value it can mean
//      (a symbol handle past its table, a depth no loop nest has) carries none,
//      and its row says why.
//   2. THE DENSITY PINS — the three shapes measured on the MIR twin (a 3.2 GB
//      table, `std::bad_alloc`, a wrap to a zero-sized table and a write past it)
//      spelled as `.dsshir`: each is refused by name and the table the reader
//      builds holds the entries the text declares, plus the unused slot 0.
//   3. THE MEANING PINS — every field whose number could be read as something
//      other than itself: the refusal, and the legitimate neighbour accepted.
//
// Red-on-disable: the helpers' range checks (`takeU32`, `takeSignedInt`, the
// label ordinal's), the density rule, the width table and each meaning rule —
// each turns its own rows red, by name.

#include "core/types/alignment.hpp"
#include "core/types/bit_int_value.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "core/types/wide_float_value.hpp"
#include "hir/attributes/shader_intrinsic.hpp"
#include "hir/attributes/source_span.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_text.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace dss;

namespace {

// ── the spellings ────────────────────────────────────────────────────────────
constexpr std::string_view kPast32  = "4294967297";              // 2^32 + 1: cut to 32 bits it is 1
constexpr std::string_view kTop32   = "4294967295";              // UINT32_MAX
constexpr std::string_view kPastI64 = "9223372036854775808";     // 2^63: as int64 it is INT64_MIN
constexpr std::string_view kBelowI64 = "-9223372036854775809";   // -(2^63) - 1: as int64 it is INT64_MAX
constexpr std::string_view kMaxI64  = "9223372036854775807";
constexpr std::string_view kMinI64  = "-9223372036854775808";
constexpr std::string_view kMaxAlignment = "2147483648";         // Alignment::kMaxBytes
static_assert(Alignment::kMaxBytes == 2147483648u,
              "the alignment control is spelled at the domain's top");

[[nodiscard]] std::string splice(std::string_view templ, std::string_view n) {
    std::string out;
    for (std::size_t i = 0; i < templ.size();) {
        if (templ.substr(i, 3) == "@N@") { out += n; i += 3; }
        else                              { out += templ[i]; ++i; }
    }
    return out;
}

[[nodiscard]] std::vector<std::string> errorsOf(DiagnosticReporter const& r) {
    std::vector<std::string> out;
    for (auto const& d : r.all()) {
        if (d.severity == DiagnosticSeverity::Error) {
            out.push_back(std::format("{}: {}", diagnosticCodeName(d.code), d.actual));
        }
    }
    return out;
}

[[nodiscard]] bool anyContains(std::vector<std::string> const& errs, std::string_view needle) {
    for (auto const& e : errs) if (e.find(needle) != std::string::npos) return true;
    return false;
}

[[nodiscard]] std::string joined(std::vector<std::string> const& errs) {
    std::string out;
    for (auto const& e : errs) { out += "\n    "; out += e; }
    return out.empty() ? std::string{" (none)"} : out;
}

// ── the module texts ─────────────────────────────────────────────────────────
constexpr std::string_view kHead = "dsshir 6\nproducer \"\"\n";
constexpr std::string_view kBuffer1 = "buffers {\n  buf 1 \"a.c\"\n}\n";
constexpr std::string_view kSymbols = "symbols {\n  %1 \"f\"\n  %2 \"g\"\n}\n";

// `function %1 : fn() -> void { block { <stmts> return void } }`.
[[nodiscard]] std::string fn(std::string_view stmts) {
    return std::string{"  function %1 : fn() -> void {\n    block {\n"} + std::string{stmts}
         + "      return void\n    }\n  }\n";
}

[[nodiscard]] std::string moduleText(std::string_view preamble, std::string_view decls) {
    return std::string{kHead} + std::string{preamble} + "module \"toy\" {\n"
         + std::string{decls} + "}\n";
}

// The first node of `kind` in pre-order, or none.
[[nodiscard]] std::optional<HirNodeId> firstOf(Hir const& hir, HirKind kind) {
    if (!hir.root().valid()) return std::nullopt;
    std::vector<HirNodeId> stack{hir.root()};
    while (!stack.empty()) {
        HirNodeId const id = stack.back();
        stack.pop_back();
        if (hir.kind(id) == kind) return id;
        auto const kids = hir.children(id);
        for (std::size_t i = kids.size(); i-- > 0;) stack.push_back(kids[i]);
    }
    return std::nullopt;
}

using ModuleProbe = std::function<std::optional<std::string>(HirParseResult const&)>;
using TypeProbe   = std::function<std::optional<std::string>(TypeInterner const&, TypeId)>;

[[nodiscard]] std::optional<std::string> str(std::uint64_t v) { return std::to_string(v); }

// The first declaration's source location / shader record.
[[nodiscard]] HirSourceLoc const* firstDeclLoc(HirParseResult const& r) {
    if (!r.hir.root().valid() || r.hir.moduleDecls(r.hir.root()).empty()) return nullptr;
    return r.sourceMap.tryGet(r.hir.moduleDecls(r.hir.root())[0]);
}
[[nodiscard]] ShaderIntrinsic const* firstDeclShader(HirParseResult const& r) {
    if (!r.hir.root().valid() || r.hir.moduleDecls(r.hir.root()).empty()) return nullptr;
    return r.shaderMap.tryGet(r.hir.moduleDecls(r.hir.root())[0]);
}
[[nodiscard]] std::optional<std::string> payloadOfFirst(HirParseResult const& r, HirKind k) {
    auto const id = firstOf(r.hir, k);
    if (!id) return std::nullopt;
    return str(r.hir.payload(*id));
}
[[nodiscard]] std::optional<TypeId> firstDeclType(HirParseResult const& r) {
    if (!r.hir.root().valid() || r.hir.moduleDecls(r.hir.root()).empty()) return std::nullopt;
    return r.hir.typeId(r.hir.moduleDecls(r.hir.root())[0]);
}

// ── 1. THE NARROWING TABLE ───────────────────────────────────────────────────

enum class Form : std::uint8_t { Module, TypeText };
enum class Width : std::uint8_t { U32, S64, Label };

struct NumberRow {
    std::string_view name;      // the gtest parameter name
    std::string_view field;     // the field, as the refusal names it
    Width            width;
    Form             form;
    std::string      templ;     // `@N@` marks the number
    // The control: the top of the range where the field legitimately takes it,
    // and how to read the value back. Empty `control` = no legitimate top value;
    // `why` then says why.
    std::string_view control;
    ModuleProbe      moduleProbe;
    TypeProbe        typeProbe;
    std::string_view why;
};

// The refusal the row must produce for `spelled`.
[[nodiscard]] std::string expectedRefusal(NumberRow const& row, std::string_view spelled) {
    switch (row.width) {
        case Width::U32:
            return std::format("{} {} does not fit its 32-bit field — it would have read "
                               "back as 1", row.field, spelled);
        case Width::Label:
            return std::format("{} L{} does not fit its 32-bit field — it would have read "
                               "back as L1", row.field, spelled);
        case Width::S64:
            return std::format("{} {} does not fit its signed 64-bit field — it would have "
                               "read back as {}", row.field, spelled,
                               spelled.starts_with('-') ? kMaxI64 : kMinI64);
    }
    return {};
}

struct Read {
    std::vector<std::string>        errors;
    std::unique_ptr<HirParseResult> module;   // Form::Module
    std::unique_ptr<TypeInterner>   interner; // Form::TypeText
    std::unique_ptr<TypeRegistry>   registry;
    TypeId                          type{};
};

[[nodiscard]] Read readRow(NumberRow const& row, std::string_view spelled) {
    Read out;
    std::string const text = splice(row.templ, spelled);
    DiagnosticReporter r;
    if (row.form == Form::Module) {
        out.module = parseHir(text, CompilationUnitId{7}, r);
    } else {
        out.interner = std::make_unique<TypeInterner>(CompilationUnitId{7});
        out.registry = std::make_unique<TypeRegistry>();
        out.type     = parseTypeFromText(text, *out.interner, *out.registry, r);
    }
    out.errors = errorsOf(r);
    return out;
}

[[nodiscard]] std::vector<NumberRow> numberRows() {
    std::string const diagCode =
        std::to_string(static_cast<std::uint32_t>(DiagnosticCode::H_TextMalformed));
    std::string const typesS  = "types {\n  type 1 = struct \"S\" {i32}\n}\n";
    auto const shaderFn = [](std::string_view attr) {
        return moduleText(kSymbols, std::string{"  @shader("} + std::string{attr} + ")\n" + fn(""));
    };
    auto const wg = [](char axis) -> ModuleProbe {
        return [axis](HirParseResult const& r) -> std::optional<std::string> {
            auto const* s = firstDeclShader(r);
            if (s == nullptr) return std::nullopt;
            return str(axis == 'x' ? s->workgroup.x : axis == 'y' ? s->workgroup.y : s->workgroup.z);
        };
    };
    std::vector<NumberRow> rows = {
        // ── the preamble ──
        {"buffer_id", "buffer id", Width::U32, Form::Module,
         moduleText("buffers {\n  buf @N@ \"a.c\"\n}\n" + std::string{kSymbols}, "  global %2 : i32\n"),
         kTop32, [](HirParseResult const& r) -> std::optional<std::string> {
             if (r.bufferNames.empty()) return std::nullopt;
             return str(r.bufferNames[0].buffer); }, {}, {}},
        {"buffer_origin", "buffer origin", Width::U32, Form::Module,
         moduleText("buffers {\n  buf 1 \"a.c\"\n  buf 2 \"a.c\" synthesized from @N@\n}\n"
                    + std::string{kSymbols}, "  global %2 : i32\n"),
         kTop32, [](HirParseResult const& r) -> std::optional<std::string> {
             if (r.bufferNames.size() < 2) return std::nullopt;
             return str(r.bufferNames[1].synthesizedMainOrigin); }, {}, {}},
        {"symbol_handle", "symbol handle", Width::U32, Form::Module,
         moduleText(kSymbols, "  global %@N@ : i32\n"), {}, {}, {},
         "a handle names an entry of the %1..%N table, so the top of the field is a "
         "handle the table does not declare"},
        // ── the attributes ──
        {"location_buffer", "location buffer", Width::U32, Form::Module,
         moduleText("buffers {\n  buf @N@ \"a.c\"\n}\n" + std::string{kSymbols},
                    "  @loc(buf @N@, 0..1)\n  global %2 : i32\n"),
         kTop32, [](HirParseResult const& r) -> std::optional<std::string> {
             auto const* l = firstDeclLoc(r);
             if (l == nullptr) return std::nullopt;
             return str(l->buffer.v); }, {}, {}},
        {"location_start_offset", "location start offset", Width::U32, Form::Module,
         moduleText(std::string{kBuffer1} + std::string{kSymbols},
                    "  @loc(buf 1, @N@..4294967295)\n  global %2 : i32\n"),
         kTop32, [](HirParseResult const& r) -> std::optional<std::string> {
             auto const* l = firstDeclLoc(r);
             if (l == nullptr) return std::nullopt;
             return str(l->span.start()); }, {}, {}},
        {"location_end_offset", "location end offset", Width::U32, Form::Module,
         moduleText(std::string{kBuffer1} + std::string{kSymbols},
                    "  @loc(buf 1, 0..@N@)\n  global %2 : i32\n"),
         kTop32, [](HirParseResult const& r) -> std::optional<std::string> {
             auto const* l = firstDeclLoc(r);
             if (l == nullptr) return std::nullopt;
             return str(l->span.end()); }, {}, {}},
        {"workgroup_size_x", "workgroup size x", Width::U32, Form::Module,
         shaderFn("stage compute, wg @N@ 1 1"), kTop32, wg('x'), {}, {}},
        {"workgroup_size_y", "workgroup size y", Width::U32, Form::Module,
         shaderFn("stage compute, wg 1 @N@ 1"), kTop32, wg('y'), {}, {}},
        {"workgroup_size_z", "workgroup size z", Width::U32, Form::Module,
         shaderFn("stage compute, wg 1 1 @N@"), kTop32, wg('z'), {}, {}},
        {"binding_set", "binding set", Width::U32, Form::Module,
         shaderFn("stage fragment, binding @N@:0"), kTop32,
         [](HirParseResult const& r) -> std::optional<std::string> {
             auto const* s = firstDeclShader(r);
             if (s == nullptr) return std::nullopt;
             return str(s->binding.set); }, {}, {}},
        {"binding_index", "binding index", Width::U32, Form::Module,
         shaderFn("stage fragment, binding 0:@N@"), kTop32,
         [](HirParseResult const& r) -> std::optional<std::string> {
             auto const* s = firstDeclShader(r);
             if (s == nullptr) return std::nullopt;
             return str(s->binding.binding); }, {}, {}},
        // UINT32_MAX is `kUnsetShaderLocation`, which the writer never spells (it
        // omits `loc`), so the control is one below it; the sentinel itself is
        // refused by its own pin below.
        {"shader_location", "shader location", Width::U32, Form::Module,
         shaderFn("stage fragment, loc @N@"), "4294967294",
         [](HirParseResult const& r) -> std::optional<std::string> {
             auto const* s = firstDeclShader(r);
             if (s == nullptr) return std::nullopt;
             return str(s->location); }, {}, {}},
        {"diagnostic_origin", "diagnostic origin", Width::U32, Form::Module,
         moduleText(kSymbols, "  @diag(code " + diagCode + ", origin @N@)\n" + fn("")), {}, {}, {},
         "an origin is the pre-order index of an earlier node, bounded by the nodes read"},
        // ── the expressions ──
        {"literal_pool_index", "literal pool index", Width::U32, Form::Module,
         moduleText(kSymbols, "  global %2 : i32 = lit #@N@ : i32\n"),
         kTop32, [](HirParseResult const& r) { return payloadOfFirst(r, HirKind::Literal); }, {}, {}},
        {"member_index", "member index", Width::U32, Form::Module,
         moduleText(typesS + std::string{kSymbols},
                    "  global %2 : type 1\n" + fn("      expr member #@N@ : i32 (ref %2 : type 1)\n")),
         {}, {}, {}, "a member index names a field of its composite, which has a handful"},
        {"swizzle_payload", "swizzle payload", Width::U32, Form::Module,
         moduleText(kSymbols, "  global %2 : vec<f32, 4>\n"
                    + fn("      expr swizzle #@N@ : vec<f32, 2> (ref %2 : vec<f32, 4>)\n")),
         {}, {}, {}, "a swizzle payload selects lanes of a short vector"},
        {"address_constant_base", "address constant base", Width::U32, Form::Module,
         moduleText(kSymbols, "  global %2 : ptr<i32> = lit addr @N@ 0 : ptr<i32>\n"),
         {}, {}, {}, "an address base names a symbol of the module's table"},
        {"bitint_literal_width", "bitint literal width", Width::U32, Form::Module,
         moduleText(kSymbols, "  global %2 : _BitInt(8) = lit bitint @N@ 0 1 5 : _BitInt(8)\n"),
         {}, {}, {}, "a width is 1..kBitIntMaxWidth; that bound has its own pin"},
        // ── the statements ──
        {"break_depth", "break depth", Width::U32, Form::Module,
         moduleText(kSymbols, fn("      while (lit int 1 : i32)\n        break @N@\n")),
         {}, {}, {}, "a depth counts enclosing loops"},
        {"continue_depth", "continue depth", Width::U32, Form::Module,
         moduleText(kSymbols, fn("      while (lit int 1 : i32)\n        continue @N@\n")),
         {}, {}, {}, "a depth counts enclosing loops"},
        {"label_ordinal", "label ordinal", Width::Label, Form::Module,
         moduleText(kSymbols, fn("      label L@N@:\n        return void\n")),
         kTop32, [](HirParseResult const& r) { return payloadOfFirst(r, HirKind::LabelStmt); }, {}, {}},
        {"inline_asm_descriptor_index", "inline-asm descriptor index", Width::U32, Form::Module,
         moduleText(kSymbols, fn("      inline_asm #@N@\n")),
         kTop32, [](HirParseResult const& r) { return payloadOfFirst(r, HirKind::InlineAsm); }, {}, {}},
        {"inline_asm_output_count", "inline-asm output count", Width::U32, Form::Module,
         moduleText(kSymbols, "  global %2 : i32\n"
                    + fn("      inline_asm \"nop\" { extended outputs @N@ operands ( \"=r\" spells "
                         "( \"%0\" ) class 1 -> ref %2 : i32 ) }\n")),
         {}, {}, {}, "an output count is at most the operands the statement lists"},
        // ── the types ──
        {"member_alignment_in_the_types_section", "member alignment", Width::U32, Form::Module,
         moduleText("types {\n  type 1 = struct \"S\" {i32 ~@N@}\n}\n" + std::string{kSymbols},
                    "  type_decl %2 : type 1\n"),
         kMaxAlignment, [](HirParseResult const& r) -> std::optional<std::string> {
             auto const t = firstDeclType(r);
             if (!t || !t->valid()) return std::nullopt;
             return str(r.interner.explicitFieldAlign(*t, 0)); }, {}, {}},
        {"bit_field_width_in_the_types_section", "bit-field width", Width::U32, Form::Module,
         moduleText("types {\n  type 1 = struct \"S\" {i32 bits @N@}\n}\n" + std::string{kSymbols},
                    "  type_decl %2 : type 1\n"),
         kTop32, [](HirParseResult const& r) -> std::optional<std::string> {
             auto const t = firstDeclType(r);
             if (!t || !t->valid()) return std::nullopt;
             auto const w = r.interner.fieldBitWidth(*t, 0);
             if (!w) return std::nullopt;
             return str(*w); }, {}, {}},
        {"member_alignment_in_a_standalone_type", "member alignment", Width::U32, Form::TypeText,
         "struct \"S\" {i32 ~@N@}", kMaxAlignment, {},
         [](TypeInterner const& in, TypeId t) -> std::optional<std::string> {
             if (!t.valid()) return std::nullopt;
             return str(in.explicitFieldAlign(t, 0)); }, {}},
        {"aligned_type_alignment", "aligned-type alignment", Width::U32, Form::TypeText,
         "aligned<i32, @N@>", kMaxAlignment, {},
         [](TypeInterner const& in, TypeId t) -> std::optional<std::string> {
             if (!t.valid()) return std::nullopt;
             return str(in.typeAlignOverride(t)); }, {}},
    };
    return rows;
}

[[nodiscard]] std::vector<NumberRow> signedRows() {
    auto const scalar = [](std::size_t i) -> TypeProbe {
        return [i](TypeInterner const& in, TypeId t) -> std::optional<std::string> {
            if (!t.valid() || in.scalars(t).size() <= i) return std::nullopt;
            return std::to_string(in.scalars(t)[i]);
        };
    };
    std::vector<NumberRow> rows = {
        {"int_literal", "int literal", Width::S64, Form::Module,
         moduleText(kSymbols, "  global %2 : i64 = lit int @N@ : i64\n"), "",
         [](HirParseResult const& r) -> std::optional<std::string> {
             if (r.literalPool.size() == 0) return std::nullopt;
             auto const* v = std::get_if<std::int64_t>(&r.literalPool.at(0).value);
             if (v == nullptr) return std::nullopt;
             return std::to_string(*v); }, {}, {}},
        {"address_constant_byte_offset", "address constant byte offset", Width::S64, Form::Module,
         moduleText(kSymbols, "  global %2 : ptr<i32> = lit addr 0 @N@ : ptr<i32>\n"), "",
         [](HirParseResult const& r) -> std::optional<std::string> {
             if (r.literalPool.size() == 0) return std::nullopt;
             auto const* v = std::get_if<HirAddressValue>(&r.literalPool.at(0).value);
             if (v == nullptr) return std::nullopt;
             return std::to_string(v->byteOffset); }, {}, {}},
        {"bitint_type_width", "_BitInt type width", Width::S64, Form::TypeText,
         "_BitInt(@N@)", {}, {}, {}, "a width is 1..kBitIntMaxWidth; its bound has its own pin"},
        {"vector_lane_count", "vector lane count", Width::S64, Form::TypeText,
         "vec<i32, @N@>", "", {}, scalar(0), {}},
        {"matrix_row_count", "matrix row count", Width::S64, Form::TypeText,
         "mat<f32, @N@, 2>", "", {}, scalar(0), {}},
        {"matrix_column_count", "matrix column count", Width::S64, Form::TypeText,
         "mat<f32, 2, @N@>", "", {}, scalar(1), {}},
        {"array_bound", "array bound", Width::S64, Form::TypeText,
         "arr<i32, @N@>", "", {}, scalar(0), {}},
        {"extension_type_scalar", "extension type scalar", Width::S64, Form::TypeText,
         "ext \"Foo\" (i32) [@N@]", "", {}, scalar(0), {}},
    };
    return rows;
}

// ⓘ A ROW WITHOUT A LEGITIMATE TOP VALUE IS NOT INSTANTIATED AS A CONTROL AT ALL,
// rather than skipped: a skip that always fires asserts nothing and looks
// deliberate. Its reason stays in the row (`why`), and the refusal half runs.
[[nodiscard]] std::vector<NumberRow> withControl(std::vector<NumberRow> rows) {
    std::erase_if(rows, [](NumberRow const& r) { return !r.why.empty(); });
    return rows;
}

class ReadsEveryUnsignedNumberAsWritten : public ::testing::TestWithParam<NumberRow> {};
class ReadsEverySignedNumberAsWritten   : public ::testing::TestWithParam<NumberRow> {};
class ReadsTheTopOfEveryUnsignedRange   : public ::testing::TestWithParam<NumberRow> {};
class ReadsBothEndsOfEverySignedRange   : public ::testing::TestWithParam<NumberRow> {};

// ONE ROW PER FIELD: spelled one past its range, the refusal names the field and
// what the number would have become.
TEST_P(ReadsEveryUnsignedNumberAsWritten, PastItsFieldIsRefusedNamingIt) {
    NumberRow const& row = GetParam();
    Read const read = readRow(row, kPast32);
    std::string const want = expectedRefusal(row, kPast32);
    EXPECT_TRUE(anyContains(read.errors, want))
        << "field `" << row.field << "` spelled " << kPast32 << " was not refused by name.\n"
        << "  wanted: " << want << "\n  got:" << joined(read.errors);
}

// AND THE CHECK IS AT THE BOUNDARY: where the field legitimately takes its top
// value, that value reads back as spelled and no range refusal names the field.
TEST_P(ReadsTheTopOfEveryUnsignedRange, ReadsBackAsWritten) {
    NumberRow const& row = GetParam();
    ASSERT_FALSE(row.control.empty()) << "a control row names no control value";
    Read const read = readRow(row, row.control);
    EXPECT_FALSE(anyContains(read.errors, "does not fit"))
        << "field `" << row.field << "` refused " << row.control << ", which fits it:"
        << joined(read.errors);
    std::optional<std::string> const got =
        row.form == Form::Module ? row.moduleProbe(*read.module)
                                 : row.typeProbe(*read.interner, read.type);
    ASSERT_TRUE(got.has_value()) << "the value of `" << row.field << "` was not read at all:"
                                 << joined(read.errors);
    EXPECT_EQ(*got, row.control) << "field `" << row.field << "` read back as another number";
}

TEST_P(ReadsEverySignedNumberAsWritten, AboveItsRangeIsRefusedNamingIt) {
    NumberRow const& row = GetParam();
    Read const read = readRow(row, kPastI64);
    std::string const want = expectedRefusal(row, kPastI64);
    EXPECT_TRUE(anyContains(read.errors, want))
        << "field `" << row.field << "` spelled " << kPastI64 << " was not refused by name.\n"
        << "  wanted: " << want << "\n  got:" << joined(read.errors);
}

TEST_P(ReadsEverySignedNumberAsWritten, BelowItsRangeIsRefusedNamingIt) {
    NumberRow const& row = GetParam();
    Read const read = readRow(row, kBelowI64);
    std::string const want = expectedRefusal(row, kBelowI64);
    EXPECT_TRUE(anyContains(read.errors, want))
        << "field `" << row.field << "` spelled " << kBelowI64 << " was not refused by name.\n"
        << "  wanted: " << want << "\n  got:" << joined(read.errors);
}

TEST_P(ReadsBothEndsOfEverySignedRange, ReadBackAsWritten) {
    NumberRow const& row = GetParam();
    for (std::string_view const extreme : {kMaxI64, kMinI64}) {
        Read const read = readRow(row, extreme);
        EXPECT_FALSE(anyContains(read.errors, "does not fit"))
            << "field `" << row.field << "` refused " << extreme << ", which fits it:"
            << joined(read.errors);
        std::optional<std::string> const got =
            row.form == Form::Module ? row.moduleProbe(*read.module)
                                     : row.typeProbe(*read.interner, read.type);
        ASSERT_TRUE(got.has_value()) << "the value of `" << row.field << "` was not read:"
                                     << joined(read.errors);
        EXPECT_EQ(*got, extreme) << "field `" << row.field << "` read back as another number";
    }
}

[[nodiscard]] std::string rowName(::testing::TestParamInfo<NumberRow> const& info) {
    return std::string{info.param.name};
}

INSTANTIATE_TEST_SUITE_P(HirText, ReadsEveryUnsignedNumberAsWritten,
                         ::testing::ValuesIn(numberRows()), rowName);
INSTANTIATE_TEST_SUITE_P(HirText, ReadsEverySignedNumberAsWritten,
                         ::testing::ValuesIn(signedRows()), rowName);
INSTANTIATE_TEST_SUITE_P(HirText, ReadsTheTopOfEveryUnsignedRange,
                         ::testing::ValuesIn(withControl(numberRows())), rowName);
INSTANTIATE_TEST_SUITE_P(HirText, ReadsBothEndsOfEverySignedRange,
                         ::testing::ValuesIn(withControl(signedRows())), rowName);

// The array bound's one NEGATIVE legitimate value, the VLA sentinel the writer
// prints verbatim, reads back as itself.
TEST(HirTextNumbers, TheVlaSentinelArrayBoundReadsBackAsItself) {
    TypeInterner in{CompilationUnitId{7}};
    TypeRegistry reg;
    DiagnosticReporter r;
    TypeId const t = parseTypeFromText("arr<i32, -2>", in, reg, r);
    ASSERT_TRUE(t.valid()) << joined(errorsOf(r));
    EXPECT_EQ(in.scalars(t)[0], -2);
}

// ── 2. THE DENSITY PINS ──────────────────────────────────────────────────────

[[nodiscard]] std::string symbolsModule(std::string_view symbolsBody) {
    return moduleText(std::string{"symbols {\n"} + std::string{symbolsBody} + "}\n", "");
}

struct DensityRead {
    std::vector<std::string>        errors;
    std::unique_ptr<HirParseResult> module;
};

[[nodiscard]] DensityRead readSymbols(std::string_view symbolsBody) {
    DensityRead out;
    DiagnosticReporter r;
    out.module = parseHir(symbolsModule(symbolsBody), CompilationUnitId{7}, r);
    out.errors = errorsOf(r);
    return out;
}

// The shapes measured on the MIR twin, as `.dsshir`: a slot far past the entries
// the text holds. Each is refused by name, and the table the reader builds holds
// the ONE entry declared plus slot 0 — never a table sized by the number.
// ✔MEASURED before the fix (MIR twin, WSL Release): `%100000000` returned a 3.2 GB
// table from a 40-byte text, `%4000000000` threw `std::bad_alloc`, `%4294967295`
// wrapped `resize(v + 1)` to 0 and wrote past it (SIGSEGV). `%4294967297` was CUT
// to `%1` by the handle reader, and `%0` is the invalid-symbol sentinel.
TEST(HirTextSymbolTable, ASlotPastTheDeclaredEntriesIsRefusedAndSizesNothing) {
    for (std::string_view const slot :
         {"100000000", "4000000000", "4294967295", "4294967297", "2", "0"}) {
        DensityRead const read = readSymbols(std::format("  %{} \"x\"\n", slot));
        std::string const want =
            std::format("symbol slot %{} is outside the 1 declared symbols", slot);
        EXPECT_TRUE(anyContains(read.errors, want))
            << "`%" << slot << "` was not refused by name:" << joined(read.errors);
        ASSERT_NE(read.module, nullptr);
        EXPECT_EQ(read.module->symbolNames.size(), 2u)
            << "`%" << slot << "` sized the symbol table by the number it spells";
        for (std::string const& name : read.module->symbolNames) {
            EXPECT_NE(name, "x") << "`%" << slot << "` landed on another slot";
        }
    }
}

TEST(HirTextSymbolTable, ASlotDeclaredTwiceIsRefused) {
    DensityRead const read = readSymbols("  %1 \"a\"\n  %1 \"b\"\n");
    EXPECT_TRUE(anyContains(read.errors, "symbol slot %1 is declared twice"))
        << joined(read.errors);
}

TEST(HirTextSymbolTable, ASecondSymbolsSectionIsRefused) {
    DiagnosticReporter r;
    auto const res = parseHir(
        moduleText("symbols {\n  %1 \"a\"\n}\nsymbols {\n  %1 \"b\"\n}\n", ""),
        CompilationUnitId{7}, r);
    EXPECT_TRUE(anyContains(errorsOf(r),
                            "a second `symbols` section — a module declares its symbols once"))
        << joined(errorsOf(r));
}

// The control: the rule is density, not order — a permutation of %1..%N reads.
TEST(HirTextSymbolTable, APermutationOfTheDenseSlotsReads) {
    DensityRead const read = readSymbols("  %2 \"b\"\n  %3 \"c\"\n  %1 \"a\"\n");
    EXPECT_TRUE(read.errors.empty()) << joined(read.errors);
    ASSERT_EQ(read.module->symbolNames.size(), 4u);
    EXPECT_EQ(read.module->symbolNames[1], "a");
    EXPECT_EQ(read.module->symbolNames[2], "b");
    EXPECT_EQ(read.module->symbolNames[3], "c");
}

// ── 3. THE MEANING PINS ──────────────────────────────────────────────────────

[[nodiscard]] std::vector<std::string> typeErrors(std::string_view text, TypeId* out = nullptr) {
    TypeInterner in{CompilationUnitId{7}};
    TypeRegistry reg;
    DiagnosticReporter r;
    TypeId const t = parseTypeFromText(text, in, reg, r);
    if (out != nullptr) *out = t;
    return errorsOf(r);
}

[[nodiscard]] std::vector<std::string> moduleErrors(std::string const& text) {
    DiagnosticReporter r;
    auto const res = parseHir(text, CompilationUnitId{7}, r);
    return errorsOf(r);
}

// An alignment the text spells is one the `Alignment` domain can hold. `~0` is
// the writer's all-or-none filler (a field with no override) and reads.
TEST(HirTextNumbers, AnAlignmentOutsideTheDomainIsRefusedByName) {
    for (std::string_view const n : {"3", "0", "4294967295"}) {
        auto const errs = typeErrors(std::format("aligned<i32, {}>", n));
        EXPECT_TRUE(anyContains(errs, std::format(
            "aligned-type alignment {} is not an alignment this build can represent", n)))
            << "aligned<i32, " << n << ">:" << joined(errs);
    }
    for (std::string_view const n : {"3", "4294967295"}) {
        auto const standalone = typeErrors(std::format("struct \"S\" {{i32 ~{}}}", n));
        EXPECT_TRUE(anyContains(standalone, std::format(
            "member alignment {} is not an alignment this build can represent", n)))
            << "struct ~" << n << ":" << joined(standalone);
        auto const section = moduleErrors(moduleText(
            std::format("types {{\n  type 1 = struct \"S\" {{i32 ~{}}}\n}}\n", n)
                + std::string{kSymbols},
            "  type_decl %2 : type 1\n"));
        EXPECT_TRUE(anyContains(section, std::format(
            "member alignment {} is not an alignment this build can represent", n)))
            << "types-section ~" << n << ":" << joined(section);
    }
    // The filler reads, in both readers.
    EXPECT_FALSE(anyContains(typeErrors("struct \"S\" {i32 ~8, i32 ~0}"), "member alignment"));
    EXPECT_FALSE(anyContains(
        moduleErrors(moduleText(
            "types {\n  type 1 = struct \"S\" {i32 ~8, i32 ~0}\n}\n" + std::string{kSymbols},
            "  type_decl %2 : type 1\n")),
        "member alignment"));
}

// The `_BitInt(N)` TYPE width is bounded by the model's own constant, and a
// refused width is refused ONCE.
TEST(HirTextNumbers, TheBitIntTypeWidthIsOneTheModelDefines) {
    std::string const top = std::to_string(kBitIntMaxWidth);
    std::string const past = std::to_string(std::uint64_t{kBitIntMaxWidth} + 1);
    for (std::string const& n : {std::string{"0"}, std::string{"-1"}, past}) {
        for (std::string_view const spelling : {"_BitInt", "unsigned _BitInt"}) {
            auto const errs = typeErrors(std::format("{}({})", spelling, n));
            EXPECT_TRUE(anyContains(errs, std::format("_BitInt width {} is not a _BitInt width", n)))
                << spelling << "(" << n << "):" << joined(errs);
            EXPECT_EQ(errs.size(), 1u) << spelling << "(" << n << ") cascaded:" << joined(errs);
        }
    }
    auto const pastI64 = typeErrors(std::format("_BitInt({})", kPastI64));
    EXPECT_EQ(pastI64.size(), 1u) << "a width refused by the range check cascaded:"
                                  << joined(pastI64);
    TypeInterner in{CompilationUnitId{7}};
    TypeRegistry reg;
    DiagnosticReporter r;
    TypeId const t = parseTypeFromText(std::format("_BitInt({})", top), in, reg, r);
    ASSERT_TRUE(t.valid()) << joined(errorsOf(r));
    EXPECT_EQ(in.bitIntWidth(t), static_cast<std::int64_t>(kBitIntMaxWidth));
}

[[nodiscard]] std::string bitintGlobal(std::string_view value, std::string_view type) {
    return moduleText(kSymbols, std::format("  global %2 : {} = lit bitint {} : {}\n",
                                            type, value, type));
}

// A `bitint` literal: the limbs are the ones the text HOLDS (a declared count
// sizes nothing — `1000000000000` limbs used to be reserved before one was read,
// 8 TB), the width is the model's, and the signedness is 0 or 1.
TEST(HirTextNumbers, ABitIntLiteralIsReadAsTheTextHoldsIt) {
    auto const undeclared = moduleErrors(bitintGlobal("64 0 1000000000000 5", "_BitInt(64)"));
    EXPECT_TRUE(anyContains(undeclared,
                            "bitint literal declares 1000000000000 limbs and the text holds 1"))
        << joined(undeclared);
    std::string const past = std::to_string(std::uint64_t{kBitIntMaxWidth} + 1);
    for (std::string const& w : {std::string{"0"}, past}) {
        auto const errs = moduleErrors(bitintGlobal(w + " 0 1 5", "_BitInt(64)"));
        EXPECT_TRUE(anyContains(errs, std::format("bitint literal width {} is not a _BitInt width", w)))
            << "width " << w << ":" << joined(errs);
    }
    auto const sign = moduleErrors(bitintGlobal("8 2 1 5", "_BitInt(8)"));
    EXPECT_TRUE(anyContains(sign, "bitint literal signedness 2 is not 0 (unsigned) or 1 (signed)"))
        << joined(sign);
    // The controls: the model's widest width, and both signednesses, read.
    std::string const top = std::to_string(kBitIntMaxWidth);
    for (std::string_view const s : {"0", "1"}) {
        auto const ok = moduleErrors(bitintGlobal(std::format("{} {} 1 5", top, s),
                                                  std::format("_BitInt({})", top)));
        EXPECT_FALSE(anyContains(ok, "bitint literal")) << "signedness " << s << ":" << joined(ok);
    }
}

// The writer never spells `kUnsetShaderLocation` (it omits `loc`); read back it
// would turn a spelled location into none.
TEST(HirTextNumbers, TheUnsetShaderLocationIsNotASpelledLocation) {
    static_assert(kUnsetShaderLocation == 4294967295u);
    auto const errs = moduleErrors(moduleText(
        kSymbols, std::string{"  @shader(stage fragment, loc 4294967295)\n"} + fn("")));
    EXPECT_TRUE(anyContains(errs, "shader location 4294967295 is the unset sentinel"))
        << joined(errs);
}

// `wfloat <bits>`: the widths the model defines round-trip, bit for bit; any
// other is refused by name, never read as an 80-bit float.
TEST(HirTextNumbers, AWideFloatWidthIsOneTheModelDefines) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const f80   = in.primitive(TypeKind::F80);
    TypeId const f128  = in.primitive(TypeKind::F128);
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const sig   = in.fnSig({}, voidT, CallConv::CcSysV);
    HirLiteralPool pool;
    HirBuilder b{"toy"};
    WideFloatValue const v80  = WideFloatValue::fromDouble(1.5, TypeKind::F80);
    WideFloatValue const v128 = WideFloatValue::fromDouble(-0.1, TypeKind::F128);
    HirNodeId stmts[] = {
        b.makeExprStmt(b.makeLiteral(f80, pool.add(HirLiteralValue{v80, TypeKind::F80}))),
        b.makeExprStmt(b.makeLiteral(f128, pool.add(HirLiteralValue{v128, TypeKind::F128}))),
        b.makeReturn(),
    };
    HirNodeId const body = b.makeBlock(stmts);
    HirNodeId const fnNode = b.makeFunction(sig, 1, {}, body);
    HirNodeId const root = b.makeModule(std::vector<HirNodeId>{fnNode});
    Hir hir = std::move(b).finish(root);
    std::vector<std::string> names{"", "f"};
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;

    DiagnosticReporter w;
    std::string const text = emitHir(hir, ctx, w);
    ASSERT_TRUE(errorsOf(w).empty()) << joined(errorsOf(w));
    EXPECT_NE(text.find("lit wfloat 80 "), std::string::npos) << text;
    EXPECT_NE(text.find("lit wfloat 128 "), std::string::npos) << text;

    DiagnosticReporter r;
    auto const res = parseHir(text, CompilationUnitId{9}, r);
    ASSERT_TRUE(res->ok) << joined(errorsOf(r));
    ASSERT_EQ(res->literalPool.size(), 2u);
    auto const* back80  = std::get_if<WideFloatValue>(&res->literalPool.at(0).value);
    auto const* back128 = std::get_if<WideFloatValue>(&res->literalPool.at(1).value);
    ASSERT_NE(back80, nullptr);
    ASSERT_NE(back128, nullptr);
    EXPECT_EQ(*back80, v80);
    EXPECT_EQ(*back128, v128);
    EXPECT_EQ(back80->kind(), TypeKind::F80);
    EXPECT_EQ(back128->kind(), TypeKind::F128);

    // A width the model does not define.
    std::string bad = text;
    std::size_t const at = bad.find("lit wfloat 80 ");
    ASSERT_NE(at, std::string::npos);
    bad.replace(at, std::string_view{"lit wfloat 80 "}.size(), "lit wfloat 64 ");
    auto const errs = moduleErrors(bad);
    EXPECT_TRUE(anyContains(errs, "wfloat width 64 is not a wide-float format this model "
                                  "defines — accepted: 80, 128"))
        << joined(errs);
}


// ── 4. THE SYMBOL A LITERAL NAMES (part 1c-b) ────────────────────────────────
//
// `HirAddressValue::base` is a SymbolId. The writer printed it RAW — `addr 2 4` —
// while every other reference in a `.dsshir` is a `%N` handle into the `symbols`
// table; the prepass never numbered it and the reader never checked it, so a
// round trip read the base as whatever symbol held HANDLE 2, and re-emitted the
// same bytes. ⓘ Latent in the product: no producer pools a folded address today
// (the const-eval engines' addresses are consumed at MIR lowering); a hand-written
// text, or the first producer that pools one, reaches it.

// A module whose raw ids and handles DISAGREE: `target` is raw 2 but the walk
// meets it first (handle 1), `p` is raw 1 and handle 2 — so the old spelling
// `addr 2 4` names `p`, its own global.
struct AddressModule {
    TypeInterner             in{CompilationUnitId{1}};
    HirLiteralPool           pool;
    std::vector<std::string> names{"", "p", "target", "", "", "", "", "only"};
    Hir                      hir{};
};

std::unique_ptr<AddressModule> addressModule(HirAddressValue base, bool nestInAggregate) {
    auto m = std::make_unique<AddressModule>();
    TypeId const i32  = m->in.primitive(TypeKind::I32);
    TypeId const pi32 = m->in.pointer(i32);
    HirBuilder b{"toy"};
    HirLiteralValue addr{base, TypeKind::Ptr};
    HirNodeId init{};
    TypeId initType = pi32;
    if (nestInAggregate) {
        // `int *p[2] = {0, &only}` — the base sits in the SECOND field of an array.
        HirAggregateValue agg;
        agg.fields.push_back(HirLiteralValue{HirAddressValue{HirAddressValue::kNullBase, 0, {}},
                                             TypeKind::Ptr});
        agg.fields.push_back(std::move(addr));
        initType = m->in.array(pi32, 2);
        init = b.makeLiteral(initType,
                             m->pool.add(HirLiteralValue{std::move(agg), TypeKind::Array}));
    } else {
        init = b.makeLiteral(pi32, m->pool.add(std::move(addr)));
    }
    HirNodeId const target = b.makeGlobal(i32, 2);
    HirNodeId const p      = b.makeGlobal(initType, 1, init);
    HirNodeId const root   = b.makeModule(std::vector<HirNodeId>{target, p});
    m->hir = std::move(b).finish(root);
    return m;
}

struct AddressRoundTrip {
    std::string                     first;
    std::string                     second;
    std::vector<std::string>        writeErrors;
    std::vector<std::string>        readErrors;
    std::unique_ptr<HirParseResult> parsed;
};

AddressRoundTrip roundTripAddress(AddressModule const& m) {
    AddressRoundTrip rt;
    HirTextContext ctx;
    ctx.interner = &m.in; ctx.symbolNames = &m.names; ctx.literalPool = &m.pool;
    DiagnosticReporter w;
    rt.first = emitHir(m.hir, ctx, w);
    rt.writeErrors = errorsOf(w);
    DiagnosticReporter r;
    rt.parsed = parseHir(rt.first, CompilationUnitId{9}, r);
    rt.readErrors = errorsOf(r);
    HirTextContext ctx2;
    ctx2.interner = &rt.parsed->interner; ctx2.symbolNames = &rt.parsed->symbolNames;
    ctx2.literalPool = &rt.parsed->literalPool;
    DiagnosticReporter w2;
    rt.second = emitHir(rt.parsed->hir, ctx2, w2);
    return rt;
}

// The address a parsed pool holds, wherever it sits (top level or a field).
std::optional<HirAddressValue> parsedAddress(HirParseResult const& r) {
    for (std::uint32_t i = 0; i < r.literalPool.size(); ++i) {
        std::vector<HirLiteralValue const*> work{&r.literalPool.at(i)};
        while (!work.empty()) {
            HirLiteralValue const* v = work.back();
            work.pop_back();
            if (auto const* a = std::get_if<HirAddressValue>(&v->value)) return *a;
            if (auto const* g = std::get_if<HirAggregateValue>(&v->value)) {
                for (auto const& f : g->fields) work.push_back(&f);
            }
        }
    }
    return std::nullopt;
}

TEST(HirTextLiteralSymbols, AnAddressBaseIsNamedThroughTheSymbolsTable) {
    auto const m = addressModule(HirAddressValue{2, 4, {}}, /*nestInAggregate=*/false);
    AddressRoundTrip const rt = roundTripAddress(*m);
    ASSERT_TRUE(rt.writeErrors.empty()) << joined(rt.writeErrors);
    EXPECT_NE(rt.first.find("lit addr %1 4 :"), std::string::npos)
        << "the base is not spelled as `target`'s handle:\n" << rt.first;
    ASSERT_TRUE(rt.parsed->ok) << joined(rt.readErrors) << "\n" << rt.first;
    auto const a = parsedAddress(*rt.parsed);
    ASSERT_TRUE(a.has_value());
    ASSERT_LT(a->base, rt.parsed->symbolNames.size());
    EXPECT_EQ(rt.parsed->symbolNames[a->base], "target")
        << "the address read back naming another symbol";
    EXPECT_EQ(a->byteOffset, 4);
    EXPECT_EQ(rt.first, rt.second);
}

// A symbol named ONLY inside an aggregate literal is numbered by the prepass and
// declared in `symbols` — the walk reaches a field.
TEST(HirTextLiteralSymbols, ASymbolNamedOnlyInsideAnAggregateIsDeclared) {
    auto const m = addressModule(HirAddressValue{7, 0, {}}, /*nestInAggregate=*/true);
    AddressRoundTrip const rt = roundTripAddress(*m);
    ASSERT_TRUE(rt.writeErrors.empty()) << joined(rt.writeErrors);
    EXPECT_NE(rt.first.find("\"only\""), std::string::npos)
        << "the symbol the aggregate names is missing from `symbols`:\n" << rt.first;
    ASSERT_TRUE(rt.parsed->ok) << joined(rt.readErrors) << "\n" << rt.first;
    auto const a = parsedAddress(*rt.parsed);
    ASSERT_TRUE(a.has_value());
    ASSERT_LT(a->base, rt.parsed->symbolNames.size());
    EXPECT_EQ(rt.parsed->symbolNames[a->base], "only");
    EXPECT_EQ(rt.first, rt.second);
}

TEST(HirTextLiteralSymbols, TheNullBaseIsSpelledZeroAndReads) {
    auto const m = addressModule(HirAddressValue{HirAddressValue::kNullBase, 16, {}}, false);
    AddressRoundTrip const rt = roundTripAddress(*m);
    ASSERT_TRUE(rt.writeErrors.empty()) << joined(rt.writeErrors);
    EXPECT_NE(rt.first.find("lit addr 0 16 :"), std::string::npos) << rt.first;
    ASSERT_TRUE(rt.parsed->ok) << joined(rt.readErrors);
    auto const a = parsedAddress(*rt.parsed);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->base, HirAddressValue::kNullBase);
    EXPECT_EQ(rt.first, rt.second);
}

// The old RAW spelling is refused by name, and an undeclared handle is refused
// by the handle reader every reference uses.
TEST(HirTextLiteralSymbols, ARawBaseAndAnUndeclaredHandleAreRefused) {
    auto const raw = moduleErrors(moduleText(
        kSymbols, "  global %2 : ptr<i32> = lit addr 5 0 : ptr<i32>\n"));
    EXPECT_TRUE(anyContains(raw, "address constant base 5 is neither a symbol `%N` nor the "
                                 "null base 0"))
        << joined(raw);
    auto const undeclared = moduleErrors(moduleText(
        kSymbols, "  global %2 : ptr<i32> = lit addr %9 0 : ptr<i32>\n"));
    EXPECT_TRUE(anyContains(undeclared, "symbol handle %9 not declared in 'symbols'"))
        << joined(undeclared);
}

// The writer asks the ONE width table too: a wide-float kind it does not hold is
// refused, never spelled 80. (A default-constructed `WideFloatValue` carries the
// inert F64 kind — the only value of the type the table has no row for.)
TEST(HirTextNumbers, AWideFloatKindWithNoWidthIsRefusedByTheWriter) {
    ASSERT_FALSE(WideFloatValue::formatBitWidth(WideFloatValue{}.kind()).has_value());
    TypeInterner in{CompilationUnitId{1}};
    TypeId const f80   = in.primitive(TypeKind::F80);
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const sig   = in.fnSig({}, voidT, CallConv::CcSysV);
    HirLiteralPool pool;
    HirBuilder b{"toy"};
    HirNodeId stmts[] = {
        b.makeExprStmt(b.makeLiteral(f80, pool.add(HirLiteralValue{WideFloatValue{}, TypeKind::F80}))),
        b.makeReturn(),
    };
    HirNodeId const body = b.makeBlock(stmts);
    HirNodeId const fnNode = b.makeFunction(sig, 1, {}, body);
    Hir hir = std::move(b).finish(b.makeModule(std::vector<HirNodeId>{fnNode}));
    std::vector<std::string> names{"", "f"};
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;
    DiagnosticReporter w;
    std::string const text = emitHir(hir, ctx, w);
    EXPECT_TRUE(anyContains(errorsOf(w), "has no `wfloat` width — accepted: 80, 128"))
        << joined(errorsOf(w)) << "\n" << text;
    EXPECT_EQ(text.find("wfloat 80"), std::string::npos) << "the kind was spelled 80:\n" << text;
}

} // namespace

namespace {

// An aggregate holding a `_BitInt` FIELD is written and read back: the field's
// core had no spelling, so the writer refused it.
TEST(HirTextLiteralSymbols, AnAggregateWithABitIntFieldRoundTrips) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const bt  = in.bitInt(70, /*isSigned=*/true);
    TypeId const arr = in.array(bt, 2);
    HirLiteralPool pool;
    HirBuilder b{"toy"};
    HirAggregateValue agg;
    BitIntValue const one(std::vector<std::uint64_t>{1u, 0u}, 70u, true);
    BitIntValue const two(std::vector<std::uint64_t>{2u, 0u}, 70u, true);
    agg.fields.push_back(HirLiteralValue{one, TypeKind::BitInt});
    agg.fields.push_back(HirLiteralValue{two, TypeKind::BitInt});
    HirNodeId const init = b.makeLiteral(arr, pool.add(HirLiteralValue{std::move(agg), TypeKind::Array}));
    HirNodeId const g = b.makeGlobal(arr, 1, init);
    Hir hir = std::move(b).finish(b.makeModule(std::vector<HirNodeId>{g}));
    std::vector<std::string> names{"", "pair"};
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;
    DiagnosticReporter w;
    std::string const text = emitHir(hir, ctx, w);
    EXPECT_TRUE(errorsOf(w).empty()) << joined(errorsOf(w)) << "\n" << text;
    DiagnosticReporter r;
    auto const back = parseHir(text, CompilationUnitId{9}, r);
    ASSERT_TRUE(back->ok) << joined(errorsOf(r)) << "\n" << text;
    HirTextContext ctx2;
    ctx2.interner = &back->interner; ctx2.symbolNames = &back->symbolNames;
    ctx2.literalPool = &back->literalPool;
    DiagnosticReporter w2;
    EXPECT_EQ(emitHir(back->hir, ctx2, w2), text);
}

// The `_Complex` twin of the bitint row (P68 round 8, lane `ht`, part 1d): a complex value in an aggregate FIELD
// carries `core == Complex`, and the HIR literal-core table had no row for it, so the writer refused the whole
// aggregate as a core "with no spelling". The C front end does not reach it today — a complex initializer stays an
// EXPRESSION in HIR (`builtincall`, `cast`, `construct`) and is folded only at MIR — so the value is fabricated here,
// the way the bitint pin fabricates its own; the MIR table's row was measured on the corpus
// (`complex_static_image`). One core, one spelling, in both tiers.
TEST(HirTextLiteralSymbols, AnAggregateWithAComplexFieldRoundTrips) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const z   = in.complex(in.primitive(TypeKind::F64));
    TypeId const arr = in.array(z, 1);
    HirLiteralPool pool;
    HirBuilder b{"toy"};
    HirAggregateValue parts;
    parts.fields.push_back(HirLiteralValue{1.5, TypeKind::F64});
    parts.fields.push_back(HirLiteralValue{-2.25, TypeKind::F64});
    HirAggregateValue agg;
    agg.fields.push_back(HirLiteralValue{std::move(parts), TypeKind::Complex});
    HirNodeId const init = b.makeLiteral(arr, pool.add(HirLiteralValue{std::move(agg), TypeKind::Array}));
    HirNodeId const g = b.makeGlobal(arr, 1, init);
    Hir hir = std::move(b).finish(b.makeModule(std::vector<HirNodeId>{g}));
    std::vector<std::string> names{"", "zs"};
    HirTextContext ctx;
    ctx.interner = &in; ctx.symbolNames = &names; ctx.literalPool = &pool;
    DiagnosticReporter w;
    std::string const text = emitHir(hir, ctx, w);
    EXPECT_TRUE(errorsOf(w).empty()) << joined(errorsOf(w)) << "\n" << text;
    EXPECT_NE(text.find(": complex"), std::string::npos) << text;
    DiagnosticReporter r;
    auto const back = parseHir(text, CompilationUnitId{9}, r);
    ASSERT_TRUE(back->ok) << joined(errorsOf(r)) << "\n" << text;
    HirTextContext ctx2;
    ctx2.interner = &back->interner; ctx2.symbolNames = &back->symbolNames;
    ctx2.literalPool = &back->literalPool;
    DiagnosticReporter w2;
    EXPECT_EQ(emitHir(back->hir, ctx2, w2), text);
}

} // namespace
