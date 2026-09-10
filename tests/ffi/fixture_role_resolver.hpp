#pragma once

// ── D-CONFIG-DESCRIPTOR-LIBRARY-LITERAL-DUPLICATES-THE-FORMAT-ROLE-TABLE ─────
//
// THE ONE test-side stand-in for the shipped object formats' `runtimeLibraries`
// tables. The shipped descriptors name their runtime images by ROLE
// (`{"role": "cLibrary"}`), and every reader that BINDS an import carries a
// resolver; a `tests/ffi/` reader must not load a format document (that tier's
// own note says so), so the tables are written here by hand.
//
// ★ WHY ONE COPY. Three files needed this fact and each grew its own: two
// verbatim `FixtureRoleResolver` classes and a third hand-written role→image
// list. Three copies of "what does `cLibrary` mean on pe" is the very shape the
// row that created the role channel exists to end — one fact, one owner — and it
// would be absurd to reproduce it in the pins for that row.
//
// ⓘ IT IS A FIXTURE, NOT A SECOND OWNER OF THE IDENTITY. What image each role
// actually denotes is pinned where it lives, against the real documents, by
// `tests/link/test_runtime_library_roles.cpp` and by the corpus guard
// `tests/link/test_descriptor_library_role_agreement.cpp`. If this table ever
// disagrees with the shipped one, the suites that read the REAL corpus through
// it go red naming the image — which is what a fixture owes.

#include "core/types/object_format_kind.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dss::ffi_test {

// The rows the shipped documents of each kind declare. A kind with no rows
// (wasm / spirv, which ship no `runtimeLibraries` table at all) answers nothing,
// exactly as its documents do.
[[nodiscard]] inline RuntimeLibraryTable
fixtureRuntimeLibraryTable(ObjectFormatKind fmt) {
    RuntimeLibraryTable t;
    switch (fmt) {
        case ObjectFormatKind::Pe:
            t.bindings.push_back({RuntimeLibraryRole::CLibrary, "ucrtbase.dll", {}});
            t.bindings.push_back(
                {RuntimeLibraryRole::SystemPrimitives, "kernel32.dll", {}});
            break;
        case ObjectFormatKind::Elf:
            t.bindings.push_back({RuntimeLibraryRole::CLibrary, "libc.so.6", {}});
            break;
        case ObjectFormatKind::MachO:
            t.bindings.push_back(
                {RuntimeLibraryRole::CLibrary, "/usr/lib/libSystem.B.dylib", {}});
            break;
        default:
            break;
    }
    return t;
}

// The image the fixture says `roleName` denotes on `fmt`, or nullopt when this
// kind declares no such row (or the spelling is not a role at all). Projected
// over the SAME table the resolver answers from, so a raw-JSON reader and a
// resolver-carrying reader can never disagree about what a role means.
[[nodiscard]] inline std::optional<std::string>
fixtureRoleImage(ObjectFormatKind fmt, std::string_view roleName) {
    auto const table = fixtureRuntimeLibraryTable(fmt);
    for (auto const& row : table.bindings) {
        if (runtimeLibraryRoleName(row.role) == roleName) return row.image;
    }
    return std::nullopt;
}

// A resolver over the fixture table for ONE kind — the shape the driver builds
// over the active format.
class FixtureRoleResolver final : public RuntimeLibraryRoleResolver {
public:
    explicit FixtureRoleResolver(ObjectFormatKind fmt)
        : kind_(objectFormatKindName(fmt)),
          table_(fixtureRuntimeLibraryTable(fmt)) {}

    [[nodiscard]] std::string_view formatKindName() const noexcept override {
        return kind_;
    }
    [[nodiscard]] RuntimeLibraryBinding const*
    rowForRole(RuntimeLibraryRole role, std::string&) const override {
        return table_.rowForRole(role);
    }

private:
    std::string_view    kind_;
    RuntimeLibraryTable table_;
};

}  // namespace dss::ffi_test
