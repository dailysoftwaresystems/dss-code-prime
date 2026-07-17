// c160 W1 -- the strongest available Mach-O reader oracle: the
// in-process WRITER->READER round-trip. Encode a REAL DSS MH_DYLIB via
// `dss::macho::encode` (the c153 export-trie writer) and read its export
// surface back with `dss::ffi::readImportsFromBytes`. The reader is the
// exact inverse of the writer, so this pins that an actual DSS dylib's
// export trie parses back to the right names + kinds.
//
// Lives in its own TU (not test_binary_reader_macho.cpp) because the
// writer headers bring `dss::SymbolVisibility` into scope, which clashes
// with `dss::ffi::SymbolVisibility` under that file's dual
// `using namespace dss;` + `using namespace dss::ffi;`. Here we import
// only `dss` and qualify the ffi types.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/binary_reader.hpp"
#include "link/format/macho.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// arm64 `RET` (C0 03 5F D6) -- a real single-instruction function body.
[[nodiscard]] std::vector<std::uint8_t> arm64Ret() {
    return {0xC0, 0x03, 0x5F, 0xD6};
}

// Exported fn `_dss_add` + exported int global `_dss_global` (.data
// {7,0,0,0}) + a LOCAL `_hidden_helper` that must NOT export -- the
// zero-import dylib witness shape (the c153 writer suite's makeExportModule).
[[nodiscard]] AssembledModule makeExportModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = arm64Ret();
    mod.functions.push_back(std::move(fn));
    AssembledFunction loc;
    loc.symbol = SymbolId{2};
    loc.bytes  = arm64Ret();
    mod.functions.push_back(std::move(loc));
    AssembledData d;
    d.symbol    = SymbolId{3};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_dss_add",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "_hidden_helper",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{3}, "_dss_global",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

[[nodiscard]] dss::ffi::ImportSurface const*
findRow(std::vector<dss::ffi::ImportSurface> const& rows,
        std::string_view name) {
    for (auto const& r : rows) if (r.mangledName == name) return &r;
    return nullptr;
}

} // namespace

TEST(BinaryReaderMachoRoundTrip, ReadsRealDssDylibExportTrie) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value()) << "loadShipped(arm64) failed";
    auto format = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-dylib");
    ASSERT_TRUE(format.has_value())
        << "loadShipped(macho64-arm64-darwin-dylib) failed";

    AssembledModule mod = makeExportModule();

    DiagnosticReporter wrep;
    auto image = dss::macho::encode(mod, *target.value(), *format.value(), wrep);
    ASSERT_EQ(wrep.errorCount(), 0u) << "dylib encode emitted errors";
    ASSERT_FALSE(image.empty());

    DiagnosticReporter rrep;
    auto r = dss::ffi::readImportsFromBytes(
        std::span<std::uint8_t const>{image.data(), image.size()},
        "libdss.dylib", rrep);
    ASSERT_TRUE(r.has_value())
        << "reader rejected the DSS dylib: "
        << (r.has_value() ? "" : r.error().detail);
    ASSERT_EQ(r->size(), 2u)
        << "exactly the two externally-visible defs export; the LOCAL "
           "_hidden_helper must stay out of the trie";

    auto const* fnRow = findRow(*r, "_dss_add");
    ASSERT_NE(fnRow, nullptr);
    EXPECT_EQ(fnRow->kind, dss::ffi::SymbolKind::Function)
        << "_dss_add lives in __text (pure instructions) -> Function";
    EXPECT_EQ(fnRow->linkage, dss::ffi::SymbolLinkage::External);
    EXPECT_EQ(fnRow->libraryPath, "libdss.dylib");

    auto const* dataRow = findRow(*r, "_dss_global");
    ASSERT_NE(dataRow, nullptr);
    EXPECT_EQ(dataRow->kind, dss::ffi::SymbolKind::Object)
        << "_dss_global lives in __DATA,__data (S_REGULAR) -> Object";

    EXPECT_EQ(findRow(*r, "_hidden_helper"), nullptr)
        << "a LOCAL symbol is never in the export trie";
    EXPECT_EQ(rrep.errorCount(), 0u);
}
