#pragma once

// ★ MACH-O'S LINKER OPTIONS, READ AS LD64 READS THEM (P69 fold 3f -- `#pragma comment`'s Mach-O half).
//
// ONE home for what one LC_LINKER_OPTION's strings ask of a link, so an object's command (the object reader) and a
// compiled unit's `#pragma comment(linker, ...)` (the backend's `applyUnitLinkerOptions`) mean one thing. ✔MEASURED
// (P69 fold 3f, Apple clang 21 + ld64): ld64 searches a library spelled as ONE string -- the document's `library`
// prefix and the name (`-lz`) -- and warns "unknown linker option from object file ignored" of the one string
// `-framework X`; it takes a framework as the TWO strings the document's `framework` word and the name. So:
//   * a library: the unit's `dependentLibraries`, once, in the order first stated;
//   * a framework: the unit's `linkerOptions`, as its strings -- a library to search too, which a link that makes an
//     image ignores for the block's reason, and a relocatable writer restates;
//   * any other: the unit's `linkerOptions` as its strings, and a warning the link that makes an image says
//     (`UnitLinkerRequests::warnings`), as ld64 says one -- a relocatable writer restates the option and says
//     nothing.

#include "core/types/unit_linker_requests.hpp"
#include "link/object_format_schema.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dss::macho {

// One option's `words` -- one LC_LINKER_OPTION's strings, or one `#pragma comment(linker, "t")` text -- read into
// `into` by `block`. `whose` begins the warning an option ld64 would not take says.
inline void readMachOLinkerOption(std::vector<std::string> words, MachOLinkerOptions const& block,
                                  UnitLinkerRequests& into, std::string_view whose) {
    if (words.size() == 1u && words.front().size() > block.library.size()
        && words.front().starts_with(block.library)) {
        std::string name = words.front().substr(block.library.size());
        auto& libs = into.dependentLibraries;
        if (std::find(libs.begin(), libs.end(), name) == libs.end()) libs.push_back(std::move(name));
        return;
    }
    bool const framework = words.size() == 2u && words.front() == block.framework && !words.back().empty();
    if (!framework) {
        into.warnings.push_back(std::format(
            "{}: the linker option '{}' is none an image's linker takes from an object -- ld64 searches a library "
            "('{}' and its name, one string) and a framework ('{}' and its name, two strings), and warns \"unknown "
            "linker option from object file ignored\" of any other: it is ignored",
            whose, spelledLinkerOption(words), block.library, block.framework));
    }
    into.linkerOptions.push_back(std::move(words));
}

}  // namespace dss::macho
