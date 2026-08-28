#include "mir/summary/mir_body_codec.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <cstring>
#include <format>
#include <utility>

namespace dss::mirsum {

namespace {

// Deliberately NOT the summary's magic. The two sections carry different
// payloads and a reader handed the wrong one must refuse on the FIRST bytes,
// not decode half a module and then disagree about a length.
constexpr char             kMagic[]   = {'D', 'S', 'S', 'M', 'I', 'R', '1', '\0'};
constexpr std::size_t      kMagicBytes = sizeof(kMagic);

void putU8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }

// LITTLE-ENDIAN, byte by byte. Never a `memcpy` of a struct: a body produced on
// the s390x leg must decode identically here.
void putU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void putU64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

void putStr(std::vector<std::uint8_t>& b, std::string_view s) {
    putU32(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

// A bounds-checked cursor. EVERY read goes through it, so a truncated buffer is
// a refusal at the read that ran off the end rather than a garbage value that
// travels one more field before it is noticed.
class Cursor {
public:
    explicit Cursor(std::span<std::uint8_t const> b) : b_(b) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    [[nodiscard]] bool match(char const* magic, std::size_t n) {
        if (!have(n)) return fail();
        bool const same = std::memcmp(b_.data() + at_, magic, n) == 0;
        at_ += n;
        if (!same) return fail();
        return true;
    }
    [[nodiscard]] std::uint8_t u8() {
        if (!have(1)) { (void)fail(); return 0; }
        return b_[at_++];
    }
    [[nodiscard]] std::uint32_t u32() {
        if (!have(4)) { (void)fail(); return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(b_[at_ + static_cast<std::size_t>(i)])
                 << (8 * i);
        at_ += 4;
        return v;
    }
    [[nodiscard]] std::uint64_t u64() {
        if (!have(8)) { (void)fail(); return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(b_[at_ + static_cast<std::size_t>(i)])
                 << (8 * i);
        at_ += 8;
        return v;
    }
    [[nodiscard]] std::string str() {
        std::uint32_t const n = u32();
        if (!ok_ || !have(n)) { (void)fail(); return {}; }
        std::string s(reinterpret_cast<char const*>(b_.data() + at_), n);
        at_ += n;
        return s;
    }
    [[nodiscard]] std::string bytes(std::uint64_t n) {
        if (!ok_ || !have(n)) { (void)fail(); return {}; }
        std::string s(reinterpret_cast<char const*>(b_.data() + at_),
                      static_cast<std::size_t>(n));
        at_ += static_cast<std::size_t>(n);
        return s;
    }
    [[nodiscard]] bool atEnd() const noexcept { return at_ == b_.size(); }

private:
    [[nodiscard]] bool have(std::uint64_t n) const noexcept {
        return ok_ && n <= b_.size() - at_;
    }
    bool fail() { ok_ = false; return false; }

    std::span<std::uint8_t const> b_;
    std::size_t                   at_ = 0;
    bool                          ok_ = true;
};

void refuse(DiagnosticReporter& reporter, std::string what) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = "decodeModuleBody: " + std::move(what)
                 + " — a body drives codegen decisions, so a misread one is a "
                   "miscompile and guessing is never the safe option "
                   "(D-OPT11-LAZY-IMPORT-EDGE).";
    reporter.report(std::move(d));
}

} // namespace

std::vector<std::uint8_t>
encodeModuleBody(Mir const& mir, TypeInterner const& interner,
                 std::span<std::string const> symbolNames,
                 std::string_view moduleDigest, std::string_view targetIdentity,
                 DiagnosticReporter& reporter) {
    // `emitMir` reports at Error severity exactly for the values it cannot
    // spell re-parseably, so the delta over this call is the honest test of
    // whether the payload can be read back.
    auto const before = reporter.errorCount();
    std::vector<std::string> const names(symbolNames.begin(), symbolNames.end());
    MirTextContext ctx;
    ctx.interner    = &interner;
    ctx.symbolNames = &names;
    std::string const text = emitMir(mir, ctx, reporter);
    if (reporter.errorCount() != before) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
        d.severity = DiagnosticSeverity::Error;
        d.actual =
            "encodeModuleBody: the module could not be rendered re-parseably, "
            "so no `.dss.mir` payload was produced. Emitting one anyway would "
            "ship a section a decoder must refuse (D-OPT11-LAZY-IMPORT-EDGE).";
        reporter.report(std::move(d));
        return {};
    }

    std::vector<std::uint8_t> b;
    b.insert(b.end(), kMagic, kMagic + kMagicBytes);
    putU32(b, kBodyFormatVersion);
    putU8(b, static_cast<std::uint8_t>(BodyPayloadKind::DssirText));
    putStr(b, moduleDigest);
    putStr(b, targetIdentity);
    putU64(b, static_cast<std::uint64_t>(text.size()));
    b.insert(b.end(), text.begin(), text.end());
    return b;
}

std::optional<DecodedModuleBody>
decodeModuleBody(std::span<std::uint8_t const> bytes, CompilationUnitId cuId,
                 std::string_view expectedDigest, std::string_view expectedTarget,
                 DiagnosticReporter& reporter) {
    Cursor c{bytes};
    if (!c.match(kMagic, kMagicBytes)) {
        refuse(reporter, "the buffer does not begin with the `.dss.mir` magic");
        return std::nullopt;
    }
    std::uint32_t const version = c.u32();
    if (!c.ok() || version != kBodyFormatVersion) {
        refuse(reporter,
               std::format("envelope version {} is not the version this reader "
                           "knows ({})",
                           version, kBodyFormatVersion));
        return std::nullopt;
    }
    std::uint8_t const kind = c.u8();
    if (!c.ok() || kind != static_cast<std::uint8_t>(BodyPayloadKind::DssirText)) {
        refuse(reporter,
               std::format("payload kind {} is not one this reader knows", kind));
        return std::nullopt;
    }
    DecodedModuleBody out;
    out.moduleDigest   = c.str();
    out.targetIdentity = c.str();
    std::uint64_t const textLen = c.u64();
    std::string const text = c.bytes(textLen);
    if (!c.ok()) {
        refuse(reporter, "the buffer is truncated");
        return std::nullopt;
    }
    if (!c.atEnd()) {
        // Trailing bytes mean the buffer is not what its header says it is.
        // Ignoring them is how a reader silently accepts two concatenated
        // payloads and decodes only the first.
        refuse(reporter, "the buffer carries bytes past the declared payload");
        return std::nullopt;
    }

    // ── the stale-body guard ────────────────────────────────────────────────
    if (!expectedDigest.empty() && out.moduleDigest != expectedDigest) {
        refuse(reporter,
               std::format("the payload carries module digest '{}' but the "
                           "caller asked for '{}' — this is a STALE body",
                           out.moduleDigest, expectedDigest));
        return std::nullopt;
    }
    if (!expectedTarget.empty() && out.targetIdentity != expectedTarget) {
        refuse(reporter,
               std::format("the payload was produced for target '{}' but the "
                           "caller asked for '{}' — importing it would splice a "
                           "body compiled for a different machine",
                           out.targetIdentity, expectedTarget));
        return std::nullopt;
    }

    auto const before = reporter.errorCount();
    out.parsed = parseMir(text, cuId, reporter);
    if (out.parsed == nullptr || !out.parsed->ok
        || reporter.errorCount() != before) {
        refuse(reporter, "the `.dssir` payload did not parse and verify");
        return std::nullopt;
    }
    return out;
}

} // namespace dss::mirsum
