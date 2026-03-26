#pragma once

#include "gen/link/targets/target_base.hpp"
#include <string>

namespace dss {

/// Windows x86_64 PE/COFF target emitter.
/// Produces valid Portable Executable binaries for 64-bit Windows.
class DSS_EXPORT TargetWindowsX86_64 : public TargetBase {
public:
    std::string name() const override { return "windows-x86_64"; }
    std::string outputExtension() const override { return ".exe"; }
    size_t pointerSize() const override { return 8; }
    Endianness endianness() const override { return Endianness::Little; }

    /// Generate a minimal Windows GUI executable that displays a message box
    /// with the given text and title, plus an OK button that closes the app.
    /// This is a standalone PE emitter — no compiler pipeline required.
    static bool generateSimpleGui(
        const std::string& outputPath,
        const std::string& message = "Hello World",
        const std::string& title = "DSS Code Prime"
    );

private:
    /// Low-level binary writer for building PE files byte-by-byte.
    class PeWriter {
    public:
        void u8(uint8_t v);
        void u16(uint16_t v);
        void u32(uint32_t v);
        void u64(uint64_t v);
        void str(const char* s);
        void zeros(size_t n);
        void padTo(size_t offset);
        size_t pos() const;
        const std::vector<uint8_t>& data() const;

    private:
        std::vector<uint8_t> buffer_;
    };

    static void writeDosHeader(PeWriter& w);
    static void writePeSignature(PeWriter& w);
    static void writeCoffHeader(PeWriter& w);
    static void writeOptionalHeader(PeWriter& w, uint32_t idataRva, uint32_t iatRva, uint32_t iatSize);
    static void writeSectionHeaders(PeWriter& w, uint32_t codeSize, uint32_t rdataSize, uint32_t idataSize);
    static void writeTextSection(PeWriter& w, uint32_t msgRva, uint32_t titleRva,
                                  uint32_t iatMessageBoxRva, uint32_t iatExitProcessRva);
    static void writeRdataSection(PeWriter& w, const std::string& message, const std::string& title);
    static void writeIdataSection(PeWriter& w);
};

} // namespace dss
