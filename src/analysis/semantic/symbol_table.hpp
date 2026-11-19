#pragma once

#include "analysis/semantic/semantic_model.hpp"
#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_id_cursor.hpp"   // IndexSymbolIds -- the one home of a fresh SymbolId
#include <cstdio>
#include <cstdlib>

#include <cstdint>
#include <utility>
#include <vector>

// SymbolId minting + SymbolId→SymbolRecord. CU-scoped: a SymbolId is
// meaningful only against the CU that produced it. Slot 0 is the
// InvalidSymbol sentinel; real ids dense 1..N.
//
// This sits between ScopeTree (name → SymbolId in a scope) and the
// SemanticModel (the SymbolRecord storage exposed to callers). It exists
// as a separate type so the analyzer never has to think about the
// invalid-slot indexing convention in line.

namespace dss {

class DSS_EXPORT SymbolTable {
public:
    SymbolTable() {
        // Slot 0 reserved for InvalidSymbol. recordFor(InvalidSymbol)
        // returns nullptr at the SemanticModel boundary.
        records_.resize(1);
    }

    SymbolTable(SymbolTable const&)            = delete;
    SymbolTable& operator=(SymbolTable const&) = delete;
    SymbolTable(SymbolTable&&)                 = default;
    SymbolTable& operator=(SymbolTable&&)      = default;

    // Mint a fresh symbol. The caller fills in the returned record by
    // SymbolId (via `at()`); the analyzer does this in one go inside
    // Pass 1 so partially-constructed records never leak.
    SymbolId mint(SymbolRecord rec) {
        // An INDEX door, the ABORT form (P69 f26e, B1): the id IS the record's index, so the table ends at the
        // first writer-reserved value -- fatal by name, never a wrapped or reserved id.
        SymbolId const id = IndexSymbolIds{records_.size()}.nextOrAbort("SymbolTable::mint", "the unit's table");
        records_.push_back(std::move(rec));
        return id;
    }

    // The id of record `position` of the table (0 < position <= size()) -- a READ through the index door, so
    // an enumeration of the table never computes an id itself.
    [[nodiscard]] SymbolId idAt(std::size_t position) const {
        return IndexSymbolIds{records_.size()}.idAt(position, "the unit's table");
    }

    [[nodiscard]] std::size_t size() const noexcept { return records_.size() - 1; }

    [[nodiscard]] SymbolRecord&       at(SymbolId id);
    [[nodiscard]] SymbolRecord const& at(SymbolId id) const;

    // Move out for SemanticModel construction. Single-use.
    [[nodiscard]] std::vector<SymbolRecord> release() && noexcept { return std::move(records_); }

    [[nodiscard]] std::vector<SymbolRecord> const& records() const noexcept { return records_; }

private:
    std::vector<SymbolRecord> records_;
};

} // namespace dss
