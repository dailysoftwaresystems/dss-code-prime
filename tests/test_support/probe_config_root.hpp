#pragma once

// ProbeConfigRoot — ONE synthetic REFERENCED language document
// (`src/dss-config/sources/probe.lang.json`) in a scratch config root that the
// process ENTERS as its cwd for the fixture's lifetime.
//
// ★ WHY A REAL FILE AND NOT A STRING. `mergeLanguageReferences` resolves a
// referenced document through `findShippedConfig` — off the filesystem, by
// logical name. There is no text entry point for the referenced side, and
// inventing one for a test would exercise a path production never takes. The
// referenced-document refusals (a `root` shape, a transitive
// `languageReferences`, a block the merge does not consume, a cross-document
// duplicate shape, a malformed `requires`) are reachable ONLY through a file the
// resolver can actually find.
//
// ★★ IT MOVES THE CWD, NOT THE ENVIRONMENT, AND THAT IS A DELIBERATE CHOICE.
// `findShippedConfig` consults `$DSS_CONFIG_ROOT` FIRST and falls THROUGH to an
// 8-ancestor cwd walk on a miss. Entering this root therefore ADDS `probe`
// without REMOVING anything: the shipped documents still resolve through
// whatever `$DSS_CONFIG_ROOT` ctest exported (or through the walk, in-tree), so
// every other arm of the calling file is untouched by construction. Overriding
// the environment instead would have taken the shipped documents away AND
// written the environment, which `config_path_walk.cpp` documents as a
// READ-only lookup whose race-freedom the project intends to keep. The cwd is
// restored in the destructor — before the tree is removed, because Windows
// refuses to delete the current directory — so it survives a gtest `ASSERT_`
// early return.
//
// ★★★ THE ROOT IS CLAIMED PER PROCESS, BY `ScratchDir`. Hoisted 2026-09-23 (P68
// round 8) from its two copies (`tests/core/test_language_references.cpp` and
// `tests/core/test_key_shape_and_text_tier_vocabulary.cpp`, the second a stated
// "second instance" of the first). Both named the root
// `temp/<prefix>-<n>` from a per-process counter and `remove_all`ed it first,
// so every concurrent process — another build tree's copy of the same binary —
// drew the SAME first name and wiped the other's probe tree. ✔MEASURED on the
// identical defect in `test_emit_hir_mode.cpp`: two concurrent instances failed
// 7 of 24 runs; every solo run passed.

#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>

namespace dss::test_support {

class ProbeConfigRoot {
public:
    // `group` only names the scratch base's subdirectory, so a leaked root says
    // which kind of test left it; uniqueness is `ScratchDir`'s, not the name's.
    explicit ProbeConfigRoot(nlohmann::json const& referencedDoc,
                             std::string_view      group = "probe-config-root")
        : scratch_(std::make_unique<ScratchDir>(Location::Temp, group)) {
        namespace fs = std::filesystem;
        std::error_code ec;
        root_ = scratch_->path();
        fs::path const sources = root_ / "src" / "dss-config" / "sources";
        fs::create_directories(sources, ec);
        if (ec) {
            ADD_FAILURE() << "could not create the probe config root: "
                          << ec.message();
            return;
        }
        {
            std::ofstream out(sources / "probe.lang.json", std::ios::binary);
            out << referencedDoc.dump(2);
        }
        previous_ = fs::current_path(ec);
        fs::current_path(root_, ec);
        if (ec) {
            ADD_FAILURE() << "could not enter the probe config root: "
                          << ec.message();
        }
    }

    ~ProbeConfigRoot() {
        std::error_code ec;
        if (!previous_.empty()) std::filesystem::current_path(previous_, ec);
        // `scratch_` (declared first, so destroyed last) removes the tree.
    }

    ProbeConfigRoot(ProbeConfigRoot const&)            = delete;
    ProbeConfigRoot& operator=(ProbeConfigRoot const&) = delete;

private:
    std::unique_ptr<ScratchDir> scratch_;
    std::filesystem::path       root_;
    std::filesystem::path       previous_;
};

} // namespace dss::test_support
