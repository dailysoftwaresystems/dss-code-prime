#pragma once

// P69 fold 3 (lane `xa`) — WHAT A TEST THAT IS NOT ABOUT THE LINK'S DOCUMENT
// HANDS THE TIERS THAT NOW READ IT, in one place.
//
// Three things grew an argument or a duty in that fold, and each reaches dozens
// of tests whose subject is something else:
//   * `mergeCuMirs`, `buildSummaryIndex` and `lazyImportOptimize` take the
//     link's document — they rank weak definitions and fold tentative ones by
//     what the document says;
//   * a weak MIR definition states its KIND (`MirLinkage::weakKind`) — the
//     builders refuse a weak binding that states none;
//   * a shipped object-format document is needed to state the first.
// A test whose subject IS one of these spells it out itself. Every other test
// says, through this header, exactly what it means: "an ordinary ELF link" and
// "a weak definition of the ordinary, overridable kind". One spelling per fact,
// so a later change to either is one edit and not sixty.

#include "core/types/symbol_attrs.hpp"
#include "link/object_format_schema.hpp"
#include "mir/mir_node.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dss::test_support {

// A shipped object-format document, loaded ONCE per name and kept for the life
// of the test binary — so a reference to it stays good however long a test
// holds it. THROWS, with the
// loader's own diagnostics, rather than hand back a null a test would then
// dereference (the `shippedSchemaOrThrow` contract, for this schema).
[[nodiscard]] inline ObjectFormatSchema const&
shippedFormatOrThrow(std::string_view name) {
    static std::mutex                                                             guard;
    static std::map<std::string, std::shared_ptr<ObjectFormatSchema>, std::less<>> loaded;
    std::lock_guard<std::mutex> const lock{guard};
    if (auto const it = loaded.find(name); it != loaded.end()) return *it->second;
    auto result = ObjectFormatSchema::loadShipped(name);
    if (!result) {
        std::string message = "ObjectFormatSchema::loadShipped(\"";
        message += name;
        message += "\") failed";
        for (auto const& d : result.error()) {
            message += "\n    ";
            message += d.path;
            message += ": ";
            message += d.message;
        }
        throw std::runtime_error(std::move(message));
    }
    if (*result == nullptr) {
        std::string message = "ObjectFormatSchema::loadShipped(\"";
        message += name;
        message += "\") returned a null schema";
        throw std::runtime_error(std::move(message));
    }
    return *loaded.emplace(std::string{name}, std::move(*result)).first->second;
}

// THE LINK DOCUMENT OF A TEST THAT IS NOT ABOUT IT: an ELF x86-64 executable
// link — the format whose statements are the ones the merge always assumed (a
// strong definition replaces every weak one, every weak definition folds at
// one rank, a common outranks a weak definition). A test that pins what a
// FORMAT says names its own document (`shippedFormatOrThrow`) and so says which
// one it means.
[[nodiscard]] inline ObjectFormatSchema const& ordinaryLinkFormat() {
    return shippedFormatOrThrow("elf64-x86_64-linux-exec");
}

// THE LINKAGE OF A TEST DEFINITION WHOSE WEAK KIND IS NOT THE SUBJECT. A weak
// definition must say which kind it is; a test about inlining, routing or
// liveness that merely needs "a weak one" means the ordinary kind every format
// has — the one a strong definition replaces (`__attribute__((weak))`). A
// binding that is not weak carries no kind, as the record requires.
[[nodiscard]] constexpr MirLinkage
linkageOf(SymbolBinding    binding,
          SymbolVisibility visibility = SymbolVisibility::Default) noexcept {
    return MirLinkage{binding, visibility,
                      binding == SymbolBinding::Weak ? WeakDefinitionKind::Overridable
                                                     : WeakDefinitionKind{}};
}

} // namespace dss::test_support
