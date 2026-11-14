#include "core/types/entry_names.hpp"

#include "core/types/assembly_config.hpp"   // AssemblyConfig::entryLabels

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace dss {

namespace {

// Each (name, verb) row of `grammar`, a language's declarations first, then a dialect's labels: the grammar's order,
// read here and nowhere else.
template <typename Take> void forEachEntryRow(GrammarSchema const& grammar, Take&& take) {
    // A language states its entry rows on its declarations, an assembly dialect -- which has none -- on its
    // `entryLabels`, in the same name-and-verb shape.
    for (auto const& decl : grammar.semantics().declarations) {
        for (auto const& row : decl.entryFunctions) take(std::string_view{row.name}, row.verb);
    }
    for (auto const& label : grammar.assembly().entryLabels) {
        for (EntryMaterialization const verb : label.verbs) take(std::string_view{label.name}, verb);
    }
}

}  // namespace

// See the header.
std::vector<RealizedEntryName> realizedEntryNames(GrammarSchema const&                  grammar,
                                                  std::span<EntryMaterialization const> realizedVerbs) {
    std::vector<RealizedEntryName> out;
    std::vector<std::size_t>       arityOf;   // parallel to `out`
    forEachEntryRow(grammar, [&](std::string_view name, EntryMaterialization verb) {
        // A row the format does not realize is no way into a program on it.
        if (std::find(realizedVerbs.begin(), realizedVerbs.end(), verb) == realizedVerbs.end()) return;
        std::size_t const arity = entryVerbParams(verb).size();
        auto const it = std::find_if(out.begin(), out.end(),
                                     [&](RealizedEntryName const& e) { return e.name == name; });
        if (it == out.end()) {
            out.push_back(RealizedEntryName{std::string{name}, verb});
            arityOf.push_back(arity);
        } else if (std::size_t const at = static_cast<std::size_t>(it - out.begin()); arity > arityOf[at]) {
            it->verb    = verb;   // the first row of the greatest arity
            arityOf[at] = arity;
        }
    });
    return out;
}

// See the header.
std::vector<EntryMaterialization> declaredEntryVerbs(GrammarSchema const& grammar) {
    std::vector<EntryMaterialization> out;
    forEachEntryRow(grammar, [&](std::string_view, EntryMaterialization verb) {
        if (std::find(out.begin(), out.end(), verb) == out.end()) out.push_back(verb);
    });
    return out;
}

}  // namespace dss
