#include "analysis/semantic/symbol_table.hpp"

#include <cstdio>
#include <cstdlib>

namespace dss {

namespace {
[[noreturn]] void stbFatal(char const* what) {
    std::fputs("dss::SymbolTable fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}
} // namespace

SymbolRecord& SymbolTable::at(SymbolId id) {
    if (!id.valid() || id.v >= records_.size()) {
        stbFatal("at(SymbolId): id out of range");
    }
    return records_[id.v];
}

SymbolRecord const& SymbolTable::at(SymbolId id) const {
    if (!id.valid() || id.v >= records_.size()) {
        stbFatal("at(SymbolId): id out of range");
    }
    return records_[id.v];
}

} // namespace dss
