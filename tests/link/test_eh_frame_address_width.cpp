// P69 fold 3f (lane `xa`, review-xa3e F3, P17): AN ABSPTR FIELD IS ONE ADDRESS OF THE OBJECT BEING READ.
//
// DW_EH_PE_absptr -- a pointer encoding whose FORMAT half is 0 -- is "the size of an address" on the target (LSB,
// "DWARF Exception Header Encoding"; LLVM's DWARFDebugFrame reads it at the object's address size). The `.eh_frame`
// decoder (link/format/dwarf_cfi_decode.hpp) read it at a literal 8 ("absptr (64-bit)"): right for every shipped
// reader, and an absptr field of an object whose addresses are 4 bytes would have been read 8 bytes wide -- the
// personality routine's field running past its augmentation data, an FDE's `initial_location` and `address_range`
// read across each other. The decoder now takes the reading object's data model and asks the pointer width's one
// home (`scalarByteSize(TypeKind::Ptr, dm)`) once; a model that states no pointer size is refused by name before a
// byte is read.
//
// A DWARF 4 CIE STATES ITS OWN ADDRESS SIZE (and a segment selector size), right after its augmentation string and
// before its code alignment factor (DWARF 4 §6.4.1; LLVM's DWARFDebugFrame reads both there). The decoder skipped
// two bytes after the return-address column instead -- the wrong place, so a version-4 CIE's factors were read from
// its address and segment sizes. It now reads both where the standard puts them, holds the stated address size to
// the reading object's, and refuses a segment selector it does not read.
//
// THE UNIVERSE: every row of the data-model table (the closed vocabulary of every shipped document's `dataModel`)
// x the field forms {absptr, a fixed 4-byte format} x the two places a pointer-encoded field lives (a CIE's
// personality, an FDE's `initial_location` and `address_range`); the two values of `DataModel` that name no model;
// a DWARF 4 CIE stating each width and a segment selector. The width each cell expects is the home's answer for its
// model, never typed per row. Each section is built here byte by byte as gcc lays out an x86_64 CIE (`def_cfa`
// the stack pointer +8, the return address at `cfa-8`), its register numbers read from the shipped target.
//
// RED-ON-DISABLE (each an arm of tests/mutations/arms.registry):
//   * the absptr arm of `dwEhPeFormatFieldWidth` back to a literal 8 -> the ILP32 cells of
//     AnAbsptrFieldIsOneAddressOfTheReadingObject (the personality field runs past its augmentation data);
//   * a DWARF 4 CIE's stated address size not compared with the reading object's -> the mismatched cells of
//     ADwarf4CieStatesItsAddressSizeBeforeItsFactorsAndIsHeldToTheReadingObject.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "link/format/dwarf_cfi.hpp"
#include "link/format/dwarf_cfi_decode.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::string diagText(DiagnosticReporter const& r) {
    std::string out = "\n  diagnostics seen (" + std::to_string(r.all().size()) + "):";
    if (r.all().empty()) out += "\n    <none>";
    for (auto const& d : r.all()) {
        out += "\n    [" + std::string{diagnosticCodeName(d.code)} + "] " + d.actual;
    }
    return out;
}

// The x86_64 registers a gcc CIE names, read from the shipped target: the stack pointer's DWARF number and the
// return-address column.
struct Frame {
    std::shared_ptr<TargetSchema> target;
    std::uint16_t                 sp = 0;
    std::uint16_t                 ra = 0;
};

[[nodiscard]] std::optional<Frame> x86Frame() {
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "the shipped x86_64 target does not load";
        return std::nullopt;
    }
    Frame f{std::move(t).value()};
    auto const regs = link::format::dwarfRegisterMappingOf(*f.target);
    if (!regs.returnAddressColumn.has_value() || *regs.returnAddressColumn >= 64u) {
        ADD_FAILURE() << "the x86_64 target declares a return-address column one DW_CFA_offset byte can name";
        return std::nullopt;
    }
    f.ra = *regs.returnAddressColumn;
    auto const rsp = std::find_if(regs.registers.begin(), regs.registers.end(),
                                  [](TargetRegisterInfo const& r) { return r.name == "rsp"; });
    if (rsp == regs.registers.end() || !rsp->dwarfNumber.has_value()) {
        ADD_FAILURE() << "the x86_64 target declares rsp with a DWARF number";
        return std::nullopt;
    }
    f.sp = *rsp->dwarfNumber;
    return f;
}

// What one section plants. The values are wider than 4 bytes and their low 4 bytes have the top bit set, so a
// field read at the wrong width, or a 4-byte unsigned field sign-extended, reads another value.
constexpr std::uint64_t kRoutine  = 0x11223344A5667788ull;
constexpr std::uint64_t kLocation = 0x01020304F5060708ull;
constexpr std::uint32_t kRange    = 0x40u;

struct Planted {
    std::uint8_t personalityEncoding = 0x00;   // the CIE's `P` byte
    std::uint8_t fdeEncoding         = 0x00;   // the CIE's `R` byte
    std::uint8_t version             = 1;
    std::uint8_t statedAddressSize   = 0;      // DWARF 4 only
    std::uint8_t segmentSelectorSize = 0;      // DWARF 4 only
};

struct Section {
    std::vector<std::uint8_t> bytes;
    std::uint32_t             personalityOffset = 0;
    std::uint32_t             locationOffset    = 0;
};

// The width a field of `enc` occupies for a reader whose addresses are `addressBytes` wide -- the LSB's table,
// stated here, not asked of the decoder.
[[nodiscard]] std::uint32_t widthOf(std::uint8_t enc, std::uint32_t addressBytes) {
    switch (enc & 0x0fu) {
    case 0x00: return addressBytes;
    case 0x03: case 0x0b: return 4;
    case 0x04: case 0x0c: return 8;
    default: ADD_FAILURE() << "the test plants only absptr, 4- and 8-byte formats"; return 0;
    }
}

[[nodiscard]] std::uint64_t lowBytes(std::uint64_t v, std::uint32_t n) {
    return n >= 8u ? v : v & ((std::uint64_t{1} << (8u * n)) - 1u);
}

void put(std::vector<std::uint8_t>& b, std::uint64_t v, std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8u * i)));
}

void putULeb(std::vector<std::uint8_t>& b, std::uint64_t v) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7fu);
        v >>= 7u;
        if (v != 0u) byte |= 0x80u;
        b.push_back(byte);
    } while (v != 0u);
}

// A record's length field, back-patched, after padding the record with DW_CFA_nop to a multiple of 8.
void closeRecord(std::vector<std::uint8_t>& b, std::size_t start) {
    while ((b.size() - start) % 8u != 0u) b.push_back(0x00);
    auto const len = static_cast<std::uint32_t>(b.size() - start - 4u);
    for (std::uint32_t i = 0; i < 4u; ++i) b[start + i] = static_cast<std::uint8_t>(len >> (8u * i));
}

// One CIE ("zPR": its personality in `p.personalityEncoding`, its FDEs' pointers in `p.fdeEncoding`) and one FDE,
// in gcc's layout, each field `widthOf` its encoding for a reader whose addresses are `addressBytes` wide.
[[nodiscard]] Section section(Frame const& f, Planted const& p, std::uint32_t addressBytes) {
    Section s;
    auto& b = s.bytes;
    std::uint32_t const pw = widthOf(p.personalityEncoding, addressBytes);
    std::uint32_t const fw = widthOf(p.fdeEncoding, addressBytes);
    put(b, 0, 4);                                       // length, back-patched
    put(b, 0, 4);                                       // CIE id
    b.push_back(p.version);
    for (char const c : std::string_view{"zPR"}) b.push_back(static_cast<std::uint8_t>(c));
    b.push_back(0);
    if (p.version == 4u) {                              // DWARF 4: address_size, segment_selector_size
        b.push_back(p.statedAddressSize);
        b.push_back(p.segmentSelectorSize);
    }
    putULeb(b, 1);                                      // code alignment factor
    b.push_back(0x78);                                  // data alignment factor, SLEB128 -8
    if (p.version == 1u) {
        b.push_back(static_cast<std::uint8_t>(f.ra));
    } else {
        putULeb(b, f.ra);
    }
    putULeb(b, 1u + pw + 1u);                           // augmentation data: P's byte and field, R's byte
    b.push_back(p.personalityEncoding);
    s.personalityOffset = static_cast<std::uint32_t>(b.size());
    put(b, lowBytes(kRoutine, pw), pw);
    b.push_back(p.fdeEncoding);
    b.push_back(0x0c);                                  // DW_CFA_def_cfa: the stack pointer + 8
    putULeb(b, f.sp);
    putULeb(b, 8);
    b.push_back(static_cast<std::uint8_t>(0x80u | f.ra));   // DW_CFA_offset: the return address at cfa-8
    putULeb(b, 1);
    closeRecord(b, 0);

    std::size_t const fde = b.size();
    put(b, 0, 4);                                       // length, back-patched
    put(b, b.size(), 4);                                // CIE pointer: from this field back to byte 0
    s.locationOffset = static_cast<std::uint32_t>(b.size());
    put(b, lowBytes(kLocation, fw), fw);                // initial_location
    put(b, kRange, fw);                                 // address_range
    putULeb(b, 0);                                      // augmentation data: the CIE has no `L`
    closeRecord(b, fde);
    put(b, 0, 4);                                       // the terminator
    return s;
}

// The value a field of `enc` holds, read back: a signed 4-byte format sign-extends, every other form does not.
[[nodiscard]] std::int64_t heldValue(std::uint64_t planted, std::uint8_t enc, std::uint32_t width) {
    std::uint64_t const raw = lowBytes(planted, width);
    if (width == 4u && (enc & 0x08u) != 0u) {
        return static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(raw)));
    }
    return static_cast<std::int64_t>(raw);
}

// Every listed model with the width its home states.
struct ListedModel {
    std::string   name;
    DataModel     model;
    std::uint32_t addressBytes;
};

[[nodiscard]] std::vector<ListedModel> listedModels() {
    std::vector<ListedModel> out;
    for (auto const& [value, name] : kDataModelTable.rows) {
        std::optional<std::uint64_t> const bytes = scalarByteSize(TypeKind::Ptr, value);
        EXPECT_TRUE(bytes.has_value()) << name << ": every listed model states a pointer size";
        if (!bytes.has_value()) continue;
        out.push_back({std::string{name}, value, static_cast<std::uint32_t>(*bytes)});
    }
    EXPECT_EQ(out.size(), kDataModelTable.rows.size());
    EXPECT_TRUE(std::any_of(out.begin(), out.end(), [](ListedModel const& m) { return m.addressBytes == 4u; }))
        << "the universe holds a model whose addresses are 4 bytes (ILP32)";
    EXPECT_TRUE(std::any_of(out.begin(), out.end(), [](ListedModel const& m) { return m.addressBytes == 8u; }))
        << "the universe holds a model whose addresses are 8 bytes (LP64, LLP64)";
    return out;
}

// Decode `s` under `model` and check the one FDE and its CIE's personality against what `p` planted.
void expectRead(Frame const& f, Section const& s, Planted const& p, DataModel model, std::uint32_t addressBytes) {
    DiagnosticReporter rep;
    auto const decoded = link::format::decodeEhFrame(s.bytes, link::format::dwarfRegisterMappingOf(*f.target), model,
                                                     "test::address-width", rep);
    ASSERT_TRUE(decoded.has_value()) << diagText(rep);
    ASSERT_EQ(decoded->fdes.size(), 1u);
    auto const&         fde = decoded->fdes.front();
    std::uint32_t const fw  = widthOf(p.fdeEncoding, addressBytes);
    std::uint32_t const pw  = widthOf(p.personalityEncoding, addressBytes);
    EXPECT_EQ(fde.initialLocationFieldOffset, s.locationOffset);
    EXPECT_EQ(fde.storedInitialLocation, heldValue(kLocation, p.fdeEncoding, fw))
        << "initial_location is " << fw << " bytes under this model";
    EXPECT_EQ(fde.cfi.codeLength, kRange) << "address_range follows initial_location at the same width";
    ASSERT_TRUE(fde.personality.has_value());
    EXPECT_EQ(fde.personality->fieldOffset, s.personalityOffset);
    EXPECT_EQ(fde.personality->width, pw) << "the personality field is " << pw << " bytes under this model";
    EXPECT_EQ(fde.personality->stored, heldValue(kRoutine, p.personalityEncoding, pw));
    EXPECT_TRUE(rep.all().empty()) << diagText(rep);
}

}  // namespace

// An absptr field -- the personality routine's, an FDE's location and range -- is one address of the reading
// object: 8 bytes under LP64 and LLP64, 4 under ILP32, and a 4-byte absptr is an unsigned address.
TEST(EhFrameAddressWidth, AnAbsptrFieldIsOneAddressOfTheReadingObject) {
    auto const f = x86Frame();
    ASSERT_TRUE(f.has_value());
    Planted const p{.personalityEncoding = 0x00, .fdeEncoding = 0x00};
    for (ListedModel const& m : listedModels()) {
        SCOPED_TRACE(m.name);
        expectRead(*f, section(*f, p, m.addressBytes), p, m.model, m.addressBytes);
    }
}

// A field of a fixed format keeps its format's width under every model: udata4 and pcrel|sdata4 are 4 bytes
// whatever the reading object's addresses are, and sdata4 sign-extends.
TEST(EhFrameAddressWidth, AFixedFormatFieldKeepsItsWidthUnderEveryModel) {
    auto const f = x86Frame();
    ASSERT_TRUE(f.has_value());
    Planted const p{.personalityEncoding = 0x03, .fdeEncoding = 0x1b};
    for (ListedModel const& m : listedModels()) {
        SCOPED_TRACE(m.name);
        expectRead(*f, section(*f, p, m.addressBytes), p, m.model, m.addressBytes);
    }
}

// A value of `DataModel` that names no model states no pointer size: the decoder reads no field at a width it was
// not told and refuses the section by name.
TEST(EhFrameAddressWidth, AModelThatStatesNoPointerSizeIsRefusedByName) {
    auto const f = x86Frame();
    ASSERT_TRUE(f.has_value());
    std::uint8_t past = 0;
    for (auto const& [value, name] : kDataModelTable.rows) {
        past = std::max<std::uint8_t>(past, static_cast<std::uint8_t>(value));
    }
    Planted const p{.personalityEncoding = 0x03, .fdeEncoding = 0x1b};
    for (std::uint8_t const raw : {std::uint8_t{0}, static_cast<std::uint8_t>(past + 1u)}) {
        SCOPED_TRACE("DataModel value " + std::to_string(raw));
        auto const model = static_cast<DataModel>(raw);
        ASSERT_FALSE(scalarByteSize(TypeKind::Ptr, model).has_value()) << "the home states none for it";
        DiagnosticReporter rep;
        auto const decoded = link::format::decodeEhFrame(section(*f, p, 4).bytes,
                                                         link::format::dwarfRegisterMappingOf(*f->target), model,
                                                         "test::address-width", rep);
        EXPECT_FALSE(decoded.has_value());
        ASSERT_EQ(rep.all().size(), 1u) << diagText(rep);
        EXPECT_EQ(rep.all().front().code, DiagnosticCode::K_UnwindRuleUnrepresentable);
        EXPECT_NE(rep.all().front().actual.find("states no pointer size"), std::string::npos) << diagText(rep);
        EXPECT_NE(rep.all().front().actual.find("(value " + std::to_string(raw) + ")"), std::string::npos)
            << "the refusal names the value" << diagText(rep);
    }
}

// A DWARF 4 CIE states its address size and its segment selector size after its augmentation string, before its
// code alignment factor. It is read there: a stated size equal to the reading object's decodes exactly; another
// size is refused by name, and so is a segment selector the decoder does not read.
TEST(EhFrameAddressWidth, ADwarf4CieStatesItsAddressSizeBeforeItsFactorsAndIsHeldToTheReadingObject) {
    auto const f = x86Frame();
    ASSERT_TRUE(f.has_value());
    for (ListedModel const& m : listedModels()) {
        SCOPED_TRACE(m.name);
        Planted p{.personalityEncoding = 0x03, .fdeEncoding = 0x1b, .version = 4};
        p.statedAddressSize = static_cast<std::uint8_t>(m.addressBytes);
        expectRead(*f, section(*f, p, m.addressBytes), p, m.model, m.addressBytes);

        auto const refused = [&](Planted const& q, std::string_view says) {
            DiagnosticReporter rep;
            auto const decoded = link::format::decodeEhFrame(section(*f, q, m.addressBytes).bytes,
                                                             link::format::dwarfRegisterMappingOf(*f->target),
                                                             m.model, "test::address-width", rep);
            EXPECT_FALSE(decoded.has_value()) << "refused, saying: " << says;
            ASSERT_EQ(rep.all().size(), 1u) << diagText(rep);
            EXPECT_EQ(rep.all().front().code, DiagnosticCode::K_UnwindRuleUnrepresentable);
            EXPECT_NE(rep.all().front().actual.find(says), std::string::npos) << diagText(rep);
        };
        Planted other = p;
        other.statedAddressSize = static_cast<std::uint8_t>(m.addressBytes == 8u ? 4u : 8u);
        refused(other, "-byte address size, and the reading object's addresses are "
                           + std::to_string(m.addressBytes) + " bytes");
        Planted segmented = p;
        segmented.segmentSelectorSize = 2;
        refused(segmented, "states a 2-byte segment selector");
    }
}
