#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Shared string-table builder for object-format walkers.
//
// Three concrete consumers (LK1 ELF / LK2 PE / LK3 Mach-O) ship with
// nearly-identical APIs but differ on the leading bytes:
//
//   * ELF (.shstrtab / .strtab) and Mach-O (LC_SYMTAB strtab): start
//     with a single NUL byte at offset 0 so `st_name=0` /
//     `n_strx=0` mean "no name". `add()` returns the offset of the
//     appended NUL-terminated string; `release()` returns the byte
//     buffer unchanged.
//
//   * PE/COFF (string table after the symbol table): starts with a
//     4-byte little-endian u32 size INCLUDING the size field itself
//     (minimum value 4 == empty table). `add()` returns the offset
//     (always >= 4). `release()` first stamps the size prefix into
//     the leading 4 bytes, then hands back the bytes.
//
// The Init policy parameter switches the two shapes. Both consumers
// have the same `add` / `view` / `release` API so the walker code is
// substrate-shaped, not format-shaped.
//
// Refactor target: simplifier review of LK2 (D-LK4-9), 3rd-consumer
// trigger (Mach-O LK3).

namespace dss::link::format::detail {

class StringTable {
public:
    enum class Init : std::uint8_t {
        // ELF and Mach-O: byte 0 = NUL; add() returns offset >= 1.
        NulByte,
        // PE/COFF: bytes 0..3 reserved for u32 size prefix; add()
        // returns offset >= 4; release() stamps the size first.
        U32SizePrefix,
    };

    explicit StringTable(Init init = Init::NulByte) : init_(init) {
        switch (init) {
        case Init::NulByte:
            bytes_.push_back(0);  // sentinel NUL at offset 0
            break;
        case Init::U32SizePrefix:
            bytes_.resize(4, 0);  // placeholder; finalized by release()
            break;
        }
    }

    // Append `name` to the table and return its byte offset. Empty
    // names return offset 0 for NulByte-init tables (the sentinel
    // NUL); PE-init tables append even empty names because there
    // is no zero-offset sentinel meaning.
    [[nodiscard]] std::uint32_t add(std::string_view name) {
        if (init_ == Init::NulByte && name.empty()) return 0;
        auto it = offsets_.find(std::string{name});
        if (it != offsets_.end()) return it->second;
        std::uint32_t const offset = static_cast<std::uint32_t>(bytes_.size());
        bytes_.insert(bytes_.end(), name.begin(), name.end());
        bytes_.push_back(0);
        offsets_.emplace(std::string{name}, offset);
        return offset;
    }

    [[nodiscard]] std::span<std::uint8_t const> view() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    // Finalize the table and extract its bytes. For PE/COFF the
    // leading 4 bytes get stamped with the total size; for NulByte
    // init it's a passthrough. Rvalue-qualified to prevent reuse
    // after drain (mirrors `DiagnosticCollector::release()`).
    [[nodiscard]] std::vector<std::uint8_t> release() && {
        if (init_ == Init::U32SizePrefix) {
            std::uint32_t const total = static_cast<std::uint32_t>(bytes_.size());
            for (int i = 0; i < 4; ++i) {
                bytes_[i] = static_cast<std::uint8_t>(total >> (i * 8));
            }
        }
        return std::move(bytes_);
    }

private:
    Init                                            init_;
    std::vector<std::uint8_t>                       bytes_;
    std::unordered_map<std::string, std::uint32_t>  offsets_;
};

} // namespace dss::link::format::detail
