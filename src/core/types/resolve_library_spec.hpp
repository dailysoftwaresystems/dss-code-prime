#pragma once

#include "core/export.hpp"

#include <filesystem>
#include <string>

// ── ResolveLibrarySpec (D-FFI-DECLARED-IMPORT-NAME) ─────────────────
//
// ONE `--resolve-library` entry: the binary to READ, plus the OPTIONAL
// runtime identity to RECORD for the symbols read out of it. The two are
// separate questions and a stand-in binary answers them differently — see
// the three-level precedence docblock on `ffi::BinaryLibrarySource`
// (`src/ffi/ingest.hpp`), which is where the ranking is decided.
//
// ONE type for the whole driver tier: the CLI parses into it
// (`program/cli_args.hpp`), the project manifest parses into it
// (`core/types/project_config.hpp`), `Program` stores it, and
// `CompileOptions` carries it to the pipeline — so no layer can drop the
// declared name in transit.
//
// ★ WHY IT LIVES IN `core` AND NOT NEXT TO THE CLI THAT NAMED IT
// (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS). It was born inside
// `program/cli_args.hpp`, and the project-config loader — which parses the
// SAME value out of a manifest — had to reach UP into the CLI header to
// name it. When that loader moved down into `core` so the editor tier could
// share it, this type had to come along or the move would only have
// relocated the upward include. It is a plain value type over two strings:
// nothing about it is CLI-specific, so `core` is where it always belonged.
//
// Source / target / object-format agnostic: two opaque strings, no arm of
// either field branches on language, CPU, or format.

namespace dss {

struct DSS_EXPORT ResolveLibrarySpec {
    // The on-disk binary whose EXPORT SURFACE is read. Always required.
    std::filesystem::path path;
    // The runtime identity to record (ELF DT_NEEDED / Mach-O LC_LOAD_DYLIB /
    // PE import-descriptor name). EMPTY = NOT STATED ⇒ the binary's own
    // embedded soname wins, else the file basename. Guaranteed non-degenerate
    // when non-empty: the CLI + manifest boundaries reject an empty side LOUD
    // rather than storing a value the loader could never resolve.
    std::string declaredImportName;
};

} // namespace dss
