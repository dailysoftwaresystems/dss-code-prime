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
//
// ★ AND OF THE ARCH, WHERE A ROLE'S IMAGE IS THE ARCH'S (P69 fold 3f, ledger
// 563 (A)). A runtime family is keyed by (kind, target arch)
// (`ObjectFormatSchema::inFlavourFamilyOf`), and on ELF the program
// interpreter differs by arch: each exec and pie document of an arch names its
// own loader, and a descriptor's arch entry (stdlib.json's aarch64
// `__stack_chk_guard`) asks for it by the role `programInterpreter`. So the
// table takes the arch the read is made on. A read with no arch decides no
// arch entry, so it never asks that role. ✔MEASURED 2026-11-17: keyed by kind
// alone, every read of the real corpus on arm64 ELF through this fixture
// refused stdlib.json ("names runtime-library role 'programInterpreter'"),
// and the sweeps that skip an unreadable descriptor dropped it silently.
[[nodiscard]] inline RuntimeLibraryTable
fixtureRuntimeLibraryTable(ObjectFormatKind fmt, std::string_view arch = {}) {
    RuntimeLibraryTable t;
    switch (fmt) {
        case ObjectFormatKind::Pe:
            t.bindings.push_back({RuntimeLibraryRole::CLibrary, "ucrtbase.dll", {}});
            t.bindings.push_back(
                {RuntimeLibraryRole::SystemPrimitives, "kernel32.dll", {}});
            break;
        case ObjectFormatKind::Elf:
            t.bindings.push_back({RuntimeLibraryRole::CLibrary, "libc.so.6", {}});
            if (arch == "x86_64") {
                t.bindings.push_back(
                    {RuntimeLibraryRole::ProgramInterpreter, "ld-linux-x86-64.so.2", {}});
            } else if (arch == "arm64") {
                t.bindings.push_back(
                    {RuntimeLibraryRole::ProgramInterpreter, "ld-linux-aarch64.so.1", {}});
            }
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
fixtureRoleImage(ObjectFormatKind fmt, std::string_view roleName,
                 std::string_view arch = {}) {
    auto const table = fixtureRuntimeLibraryTable(fmt, arch);
    for (auto const& row : table.bindings) {
        if (runtimeLibraryRoleName(row.role) == roleName) return row.image;
    }
    return std::nullopt;
}

// A resolver over the fixture table for ONE kind on one arch — the shape the
// driver builds over the active format (whose family is its kind on its arch).
class FixtureRoleResolver final : public RuntimeLibraryRoleResolver {
public:
    explicit FixtureRoleResolver(ObjectFormatKind fmt, std::string_view arch = {})
        : kind_(objectFormatKindName(fmt)),
          table_(fixtureRuntimeLibraryTable(fmt, arch)) {}

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
