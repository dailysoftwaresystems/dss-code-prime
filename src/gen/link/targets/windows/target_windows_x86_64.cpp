#include "gen/link/targets/windows/target_windows_x86_64.hpp"
#include <cstring>
#include <fstream>

namespace dss {

// ═══════════════════════════════════════════════════════════════════════════════
// PeWriter — Low-level byte writer
// ═══════════════════════════════════════════════════════════════════════════════

void TargetWindowsX86_64::PeWriter::u8(uint8_t v) { buffer_.push_back(v); }
void TargetWindowsX86_64::PeWriter::u16(uint16_t v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
void TargetWindowsX86_64::PeWriter::u32(uint32_t v) { u16(v & 0xFFFF); u16((v >> 16) & 0xFFFF); }
void TargetWindowsX86_64::PeWriter::u64(uint64_t v) { u32(v & 0xFFFFFFFF); u32((v >> 32) & 0xFFFFFFFF); }
void TargetWindowsX86_64::PeWriter::str(const char* s) {
    auto len = std::strlen(s) + 1;
    buffer_.insert(buffer_.end(), s, s + len);
}
void TargetWindowsX86_64::PeWriter::zeros(size_t n) { buffer_.insert(buffer_.end(), n, 0); }
void TargetWindowsX86_64::PeWriter::padTo(size_t offset) {
    if (buffer_.size() < offset) zeros(offset - buffer_.size());
}
size_t TargetWindowsX86_64::PeWriter::pos() const { return buffer_.size(); }
const std::vector<uint8_t>& TargetWindowsX86_64::PeWriter::data() const { return buffer_; }

// ═══════════════════════════════════════════════════════════════════════════════
// PE Layout Constants
// ═══════════════════════════════════════════════════════════════════════════════
//
// File layout (512-byte aligned):
//   0x000 - 0x1FF : Headers (DOS + PE + COFF + Optional + Section headers)
//   0x200 - 0x3FF : .text   (executable code)
//   0x400 - 0x5FF : .rdata  (read-only string constants)
//   0x600 - 0x7FF : .idata  (import directory, IAT, DLL names)
//
// Memory layout (4KB aligned, ImageBase = 0x400000):
//   0x0000 - 0x0FFF : Headers
//   0x1000 - 0x1FFF : .text   (RVA 0x1000)
//   0x2000 - 0x2FFF : .rdata  (RVA 0x2000)
//   0x3000 - 0x3FFF : .idata  (RVA 0x3000)

static constexpr uint32_t FILE_ALIGNMENT    = 0x200;
static constexpr uint32_t SECTION_ALIGNMENT = 0x1000;
static constexpr uint64_t IMAGE_BASE        = 0x00400000;

static constexpr uint32_t TEXT_RVA   = 0x1000;
static constexpr uint32_t RDATA_RVA  = 0x2000;
static constexpr uint32_t IDATA_RVA  = 0x3000;

static constexpr uint32_t TEXT_FILE_OFFSET  = 0x200;
static constexpr uint32_t RDATA_FILE_OFFSET = 0x400;
static constexpr uint32_t IDATA_FILE_OFFSET = 0x600;

// .idata section internal layout (offsets relative to IDATA_RVA):
//
//   +0x00  Import Directory Table (IDT): 3 entries × 20 bytes = 60 bytes
//          [0] user32.dll   [1] kernel32.dll   [2] null terminator
//
//   +0x3C  Import Lookup Table (ILT) for user32.dll:    2 × 8 = 16 bytes
//   +0x4C  Import Lookup Table (ILT) for kernel32.dll:  2 × 8 = 16 bytes
//
//   +0x5C  Import Address Table (IAT) for user32.dll:   2 × 8 = 16 bytes
//   +0x6C  Import Address Table (IAT) for kernel32.dll: 2 × 8 = 16 bytes
//
//   +0x7C  Hint/Name: "MessageBoxA"  (2 + 12 = 14 bytes)
//   +0x8A  Hint/Name: "ExitProcess"  (2 + 12 = 14 bytes)
//
//   +0x98  DLL name: "user32.dll\0"   (11 bytes)
//   +0xA3  DLL name: "kernel32.dll\0" (13 bytes)

static constexpr uint32_t IDT_OFF         = 0x00;
static constexpr uint32_t ILT_USER32      = 0x3C;
static constexpr uint32_t ILT_KERNEL32    = 0x4C;
static constexpr uint32_t IAT_USER32      = 0x5C;
static constexpr uint32_t IAT_KERNEL32    = 0x6C;
static constexpr uint32_t HN_MESSAGEBOX   = 0x7C;
static constexpr uint32_t HN_EXITPROCESS  = 0x8A;
static constexpr uint32_t NAME_USER32     = 0x98;
static constexpr uint32_t NAME_KERNEL32   = 0xA3;
static constexpr uint32_t IDATA_USED_SIZE = 0xB0;

// IAT absolute RVAs (used by code for call [rip+disp] instructions)
static constexpr uint32_t IAT_MESSAGEBOX_RVA  = IDATA_RVA + IAT_USER32;     // 0x305C
static constexpr uint32_t IAT_EXITPROCESS_RVA = IDATA_RVA + IAT_KERNEL32;   // 0x306C

// ═══════════════════════════════════════════════════════════════════════════════
// DOS Header
// ═══════════════════════════════════════════════════════════════════════════════

void TargetWindowsX86_64::writeDosHeader(PeWriter& w) {
    w.u16(0x5A4D);      // e_magic: "MZ"
    w.zeros(58);         // Unused DOS header fields (offsets 0x02–0x3B)
    w.u32(0x80);         // e_lfanew: PE signature is at file offset 0x80
    w.padTo(0x80);       // Zero-fill DOS stub area
}

// ═══════════════════════════════════════════════════════════════════════════════
// PE Signature
// ═══════════════════════════════════════════════════════════════════════════════

void TargetWindowsX86_64::writePeSignature(PeWriter& w) {
    w.u32(0x00004550);   // "PE\0\0"
}

// ═══════════════════════════════════════════════════════════════════════════════
// COFF File Header (20 bytes)
// ═══════════════════════════════════════════════════════════════════════════════

void TargetWindowsX86_64::writeCoffHeader(PeWriter& w) {
    w.u16(0x8664);       // Machine: IMAGE_FILE_MACHINE_AMD64
    w.u16(3);            // NumberOfSections: .text, .rdata, .idata
    w.u32(0);            // TimeDateStamp
    w.u32(0);            // PointerToSymbolTable
    w.u32(0);            // NumberOfSymbols
    w.u16(240);          // SizeOfOptionalHeader (PE32+ = 240 bytes)
    w.u16(0x0022);       // Characteristics: EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
}

// ═══════════════════════════════════════════════════════════════════════════════
// Optional Header PE32+ (240 bytes = 24 standard + 88 Windows + 128 data dirs)
// ═══════════════════════════════════════════════════════════════════════════════

void TargetWindowsX86_64::writeOptionalHeader(
    PeWriter& w, uint32_t idataRva, uint32_t iatRva, uint32_t iatSize
) {
    // ── Standard fields (24 bytes) ──
    w.u16(0x020B);             // Magic: PE32+ (64-bit)
    w.u8(14); w.u8(0);        // Linker version 14.0
    w.u32(FILE_ALIGNMENT);     // SizeOfCode
    w.u32(FILE_ALIGNMENT * 2); // SizeOfInitializedData (.rdata + .idata)
    w.u32(0);                  // SizeOfUninitializedData
    w.u32(TEXT_RVA);           // AddressOfEntryPoint
    w.u32(TEXT_RVA);           // BaseOfCode

    // ── Windows-specific fields (88 bytes) ──
    w.u64(IMAGE_BASE);        // ImageBase
    w.u32(SECTION_ALIGNMENT); // SectionAlignment
    w.u32(FILE_ALIGNMENT);    // FileAlignment
    w.u16(6); w.u16(0);       // OS version 6.0 (Vista+)
    w.u16(0); w.u16(0);       // Image version 0.0
    w.u16(6); w.u16(0);       // Subsystem version 6.0
    w.u32(0);                  // Win32VersionValue (reserved)
    w.u32(0x4000);             // SizeOfImage (headers + 3 sections × 0x1000)
    w.u32(FILE_ALIGNMENT);     // SizeOfHeaders (all headers fit in 0x200)
    w.u32(0);                  // CheckSum
    w.u16(2);                  // Subsystem: IMAGE_SUBSYSTEM_WINDOWS_GUI
    w.u16(0x8100);             // DllCharacteristics: NX_COMPAT | TERMINAL_SERVER_AWARE
    w.u64(0x100000);           // SizeOfStackReserve  (1 MB)
    w.u64(0x1000);             // SizeOfStackCommit   (4 KB)
    w.u64(0x100000);           // SizeOfHeapReserve   (1 MB)
    w.u64(0x1000);             // SizeOfHeapCommit    (4 KB)
    w.u32(0);                  // LoaderFlags
    w.u32(16);                 // NumberOfRvaAndSizes

    // ── Data Directories (16 entries × 8 bytes = 128 bytes) ──
    w.u64(0);                                     //  [0] Export Table
    w.u32(idataRva); w.u32(60);                   //  [1] Import Table (3 descriptors × 20)
    w.u64(0);                                     //  [2] Resource Table
    w.u64(0);                                     //  [3] Exception Table
    w.u64(0);                                     //  [4] Certificate Table
    w.u64(0);                                     //  [5] Base Relocation Table
    w.u64(0);                                     //  [6] Debug
    w.u64(0);                                     //  [7] Architecture
    w.u64(0);                                     //  [8] Global Ptr
    w.u64(0);                                     //  [9] TLS Table
    w.u64(0);                                     // [10] Load Config
    w.u64(0);                                     // [11] Bound Import
    w.u32(iatRva); w.u32(iatSize);                // [12] IAT
    w.u64(0);                                     // [13] Delay Import
    w.u64(0);                                     // [14] CLR Runtime
    w.u64(0);                                     // [15] Reserved
}

// ═══════════════════════════════════════════════════════════════════════════════
// Section Headers (3 × 40 bytes)
// ═══════════════════════════════════════════════════════════════════════════════

void TargetWindowsX86_64::writeSectionHeaders(
    PeWriter& w, uint32_t codeSize, uint32_t rdataSize, uint32_t idataSize
) {
    // .text — executable code
    w.str(".text");  w.padTo(w.pos() + (8 - 6));  // 8-byte padded name
    w.u32(codeSize);                                // VirtualSize
    w.u32(TEXT_RVA);                                // VirtualAddress
    w.u32(FILE_ALIGNMENT);                          // SizeOfRawData
    w.u32(TEXT_FILE_OFFSET);                        // PointerToRawData
    w.u32(0); w.u32(0); w.u16(0); w.u16(0);        // Relocations / line numbers
    w.u32(0x60000020);                              // CODE | EXECUTE | READ

    // .rdata — read-only data (string constants)
    w.str(".rdata"); w.padTo(w.pos() + (8 - 7));   // 8-byte padded name
    w.u32(rdataSize);                               // VirtualSize
    w.u32(RDATA_RVA);                               // VirtualAddress
    w.u32(FILE_ALIGNMENT);                          // SizeOfRawData
    w.u32(RDATA_FILE_OFFSET);                       // PointerToRawData
    w.u32(0); w.u32(0); w.u16(0); w.u16(0);
    w.u32(0x40000040);                              // INITIALIZED_DATA | READ

    // .idata — import tables (writable: loader patches IAT at load time)
    w.str(".idata"); w.padTo(w.pos() + (8 - 7));   // 8-byte padded name
    w.u32(idataSize);                               // VirtualSize
    w.u32(IDATA_RVA);                               // VirtualAddress
    w.u32(FILE_ALIGNMENT);                          // SizeOfRawData
    w.u32(IDATA_FILE_OFFSET);                       // PointerToRawData
    w.u32(0); w.u32(0); w.u16(0); w.u16(0);
    w.u32(0xC0000040);                              // INITIALIZED_DATA | READ | WRITE
}

// ═══════════════════════════════════════════════════════════════════════════════
// .text Section — x86_64 Machine Code
// ═══════════════════════════════════════════════════════════════════════════════
//
// The generated code does:
//   MessageBoxA(NULL, message, title, MB_OK);
//   ExitProcess(0);
//
// All addresses use RIP-relative addressing for position independence.
// Windows x64 calling convention: RCX, RDX, R8, R9 + 32-byte shadow space.

void TargetWindowsX86_64::writeTextSection(
    PeWriter& w, uint32_t msgRva, uint32_t titleRva,
    uint32_t iatMessageBoxRva, uint32_t iatExitProcessRva
) {
    const size_t sectionStart = w.pos();
    const uint32_t codeRva = TEXT_RVA;

    // Track the RVA of the *next* instruction for RIP-relative displacement calculation.
    // displacement = target_rva - next_instruction_rva
    uint32_t rva = codeRva;

    // sub rsp, 0x28  — allocate shadow space + align stack to 16 bytes
    w.u8(0x48); w.u8(0x83); w.u8(0xEC); w.u8(0x28);
    rva += 4;

    // xor ecx, ecx  — hWnd = NULL
    w.u8(0x31); w.u8(0xC9);
    rva += 2;

    // lea rdx, [rip + disp]  — lpText = message string
    w.u8(0x48); w.u8(0x8D); w.u8(0x15);
    rva += 3;  // RIP points here (after the 3 opcode bytes)
    uint32_t nextRip = rva + 4;  // after the 4-byte displacement
    w.u32(msgRva - nextRip);
    rva = nextRip;

    // lea r8, [rip + disp]  — lpCaption = title string
    w.u8(0x4C); w.u8(0x8D); w.u8(0x05);
    rva += 3;
    nextRip = rva + 4;
    w.u32(titleRva - nextRip);
    rva = nextRip;

    // xor r9d, r9d  — uType = MB_OK (0)
    w.u8(0x45); w.u8(0x31); w.u8(0xC9);
    rva += 3;

    // call [rip + disp]  — MessageBoxA via IAT
    w.u8(0xFF); w.u8(0x15);
    rva += 2;
    nextRip = rva + 4;
    w.u32(iatMessageBoxRva - nextRip);
    rva = nextRip;

    // xor ecx, ecx  — exit code = 0
    w.u8(0x31); w.u8(0xC9);
    rva += 2;

    // call [rip + disp]  — ExitProcess via IAT
    w.u8(0xFF); w.u8(0x15);
    rva += 2;
    nextRip = rva + 4;
    w.u32(iatExitProcessRva - nextRip);
    rva = nextRip;

    // Pad section to FILE_ALIGNMENT
    w.padTo(sectionStart + FILE_ALIGNMENT);
}

// ═══════════════════════════════════════════════════════════════════════════════
// .rdata Section — Read-Only String Constants
// ═══════════════════════════════════════════════════════════════════════════════

void TargetWindowsX86_64::writeRdataSection(
    PeWriter& w, const std::string& message, const std::string& title
) {
    const size_t sectionStart = w.pos();

    // Message string at offset 0x00 (RVA 0x2000)
    w.str(message.c_str());

    // Align to 16-byte boundary for title
    w.padTo(sectionStart + 0x10);

    // Title string at offset 0x10 (RVA 0x2010)
    w.str(title.c_str());

    // Pad section to FILE_ALIGNMENT
    w.padTo(sectionStart + FILE_ALIGNMENT);
}

// ═══════════════════════════════════════════════════════════════════════════════
// .idata Section — Import Directory, Lookup/Address Tables, Hint/Names
// ═══════════════════════════════════════════════════════════════════════════════

void TargetWindowsX86_64::writeIdataSection(PeWriter& w) {
    const size_t sectionStart = w.pos();

    // ── Import Directory Table (IDT): 3 entries × 20 bytes ──

    // [0] user32.dll
    w.u32(IDATA_RVA + ILT_USER32);     // OriginalFirstThunk → ILT
    w.u32(0);                           // TimeDateStamp
    w.u32(0);                           // ForwarderChain
    w.u32(IDATA_RVA + NAME_USER32);    // Name → "user32.dll"
    w.u32(IDATA_RVA + IAT_USER32);     // FirstThunk → IAT

    // [1] kernel32.dll
    w.u32(IDATA_RVA + ILT_KERNEL32);
    w.u32(0);
    w.u32(0);
    w.u32(IDATA_RVA + NAME_KERNEL32);
    w.u32(IDATA_RVA + IAT_KERNEL32);

    // [2] Null terminator
    w.zeros(20);

    // ── Import Lookup Tables (ILT) ──

    // ILT for user32.dll (2 entries × 8 bytes)
    w.padTo(sectionStart + ILT_USER32);
    w.u64(IDATA_RVA + HN_MESSAGEBOX);  // → Hint/Name for MessageBoxA
    w.u64(0);                           // Null terminator

    // ILT for kernel32.dll (2 entries × 8 bytes)
    w.padTo(sectionStart + ILT_KERNEL32);
    w.u64(IDATA_RVA + HN_EXITPROCESS); // → Hint/Name for ExitProcess
    w.u64(0);                           // Null terminator

    // ── Import Address Tables (IAT) — identical to ILT; loader overwrites at runtime ──

    // IAT for user32.dll
    w.padTo(sectionStart + IAT_USER32);
    w.u64(IDATA_RVA + HN_MESSAGEBOX);
    w.u64(0);

    // IAT for kernel32.dll
    w.padTo(sectionStart + IAT_KERNEL32);
    w.u64(IDATA_RVA + HN_EXITPROCESS);
    w.u64(0);

    // ── Hint/Name Table ──

    // MessageBoxA
    w.padTo(sectionStart + HN_MESSAGEBOX);
    w.u16(0);                           // Hint (ordinal hint, 0 = let loader search)
    w.str("MessageBoxA");

    // ExitProcess
    w.padTo(sectionStart + HN_EXITPROCESS);
    w.u16(0);
    w.str("ExitProcess");

    // ── DLL Name Strings ──

    w.padTo(sectionStart + NAME_USER32);
    w.str("user32.dll");

    w.padTo(sectionStart + NAME_KERNEL32);
    w.str("kernel32.dll");

    // Pad section to FILE_ALIGNMENT
    w.padTo(sectionStart + FILE_ALIGNMENT);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Public API: Generate a complete PE executable
// ═══════════════════════════════════════════════════════════════════════════════

bool TargetWindowsX86_64::generateSimpleGui(
    const std::string& outputPath,
    const std::string& message,
    const std::string& title
) {
    PeWriter w;

    // Strings are placed at fixed offsets within .rdata
    const uint32_t msgRva   = RDATA_RVA + 0x00;   // "Hello World" at start
    const uint32_t titleRva = RDATA_RVA + 0x10;   // Title at +0x10

    // Code size = 37 bytes (0x25)
    const uint32_t codeSize  = 0x25;
    const uint32_t rdataSize = 0x10 + static_cast<uint32_t>(title.size()) + 1;
    const uint32_t idataSize = IDATA_USED_SIZE;

    // IAT spans both user32 and kernel32 tables (contiguous, 32 bytes total)
    const uint32_t iatRva  = IDATA_RVA + IAT_USER32;
    const uint32_t iatSize = 32;  // 2 tables × 16 bytes

    // ── Build the PE file ──

    writeDosHeader(w);                                                  // 0x000
    writePeSignature(w);                                                // 0x080
    writeCoffHeader(w);                                                 // 0x084
    writeOptionalHeader(w, IDATA_RVA, iatRva, iatSize);                // 0x098
    writeSectionHeaders(w, codeSize, rdataSize, idataSize);            // 0x188
    w.padTo(TEXT_FILE_OFFSET);                                          // 0x200

    writeTextSection(w, msgRva, titleRva,
                     IAT_MESSAGEBOX_RVA, IAT_EXITPROCESS_RVA);         // 0x200
    writeRdataSection(w, message, title);                               // 0x400
    writeIdataSection(w);                                               // 0x600

    // ── Write to disk ──

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) return false;

    const auto& data = w.data();
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));

    return file.good();
}

} // namespace dss
