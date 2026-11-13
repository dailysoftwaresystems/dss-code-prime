#pragma once

// Every shipped RELOCATABLE-object document (neither an image nor an archive), with its target -- the shipped
// vocabulary a pin over "every object format" walks (P69 H6b, the review of H6's N2: a hand-typed target list left
// both Mach-O documents out with no reason). ONE table for every such pin, CENSUSED against the shipped config by
// `ArchivePackaging.TheTablesHoldEveryShippedArchiveAndRelocatableDocument` (tests/program/
// test_archive_definition_visibility.cpp): a document added to src/dss-config/object-formats fails THERE until its
// row is written here, so no pin that walks this table can silently miss it.

#include <string>

namespace dss::test_support {

struct RelocatableDocument {
    char const* arch;     // the target
    char const* format;   // the relocatable document

    // The `<arch>:<format>` spelling a `--target` and `Program::compileUnits` take.
    [[nodiscard]] std::string target() const { return std::string{arch} + ":" + format; }
};

inline constexpr RelocatableDocument kRelocatableDocuments[] = {
    {"x86_64", "elf64-x86_64-linux"},  {"x86_64", "pe64-x86_64-windows"}, {"x86_64", "macho64-x86_64-darwin"},
    {"arm64", "elf64-aarch64-linux"}, {"arm64", "macho64-arm64-darwin"},
};

}  // namespace dss::test_support
