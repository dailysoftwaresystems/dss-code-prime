// THE RAW ENTRY OF A PROGRAM A REFERENCE PE LINKER BUILDS WITH NO C RUNTIME STARTUP.
//
// WHY IT EXISTS. A test that hands DSS objects to link.exe, lld-link or GNU ld keeps the C runtime's startup
// out of the image (`/NODEFAULTLIB /ENTRY:<symbol>`, `-nostartfiles -e <symbol>`) so that the subject is the
// DSS objects alone. The image's entry is then whatever function the test names -- and RETURNING from a raw
// PE entry ends the THREAD, not the process: the process lives until its last thread ends, and exits with
// THAT thread's code. Nothing calls `ExitProcess` for such an image; the startup the test left out is what
// does.
//
// ✔MEASURED 2026-10-08 (Windows 11; lld-link 19.1.5, link.exe 14.44.35228, GNU ld 2.42; the two DSS objects
// of `test_pe_object_function_address_reference_link.cpp`, whose image imports from KERNEL32.dll and the
// ucrt's stdio api set and calls LoadLibraryA("ucrtbase.dll")):
//   * with `main` as the raw entry, 3 of 320 runs took 30.1-30.5 s and exited 0 where the program returns 42
//     (lld-link 2 of 110, GNU ld 1 of 100, link.exe 0 of 110). Against the suite's run budget (5000 ms) that
//     is a timeout, which is how a gate run met it; a solo run takes 57-70 ms;
//   * the same program ENDING IN `ExitProcess`: 0 of 300;
//   * the mechanism, shown on purpose: a raw-entry program that starts a thread which sleeps 3000 ms and
//     returns 7, and then returns 42 itself, takes 3.06-3.13 s and exits 7 under lld-link and link.exe; ending
//     in `ExitProcess(42)` it takes about 50 ms and exits 42;
//   * with the entry below, the subject objects unchanged: 0 of 480 under the three linkers, sixteen at a
//     time, every exit 42.
// INFERRED, not measured: WHICH threads such a process waited for -- the loader's parallel-load workers,
// whose idle timeout is 30 s. It is consistent with the measurement that only an image importing from a
// SECOND dll ever waited: a returning image importing from kernel32 alone measured 0 of 480, and one
// importing nothing 0 of 320.
//
// SO a program a test links this way starts at `kSymbol`, which one more DSS-compiled object of `kSource`
// defines: it calls the program's `main` and ends the PROCESS with what `main` returns. The subject objects
// and everything they state are untouched; the link line gains that object, and kernel32 where it had not
// named it. (`test_pe_object_data_import_slot.cpp` keeps a returning raw `main`: its programs import
// NOTHING by design, which is the shape that never waited, and an import there would defeat its subject.)

#pragma once

namespace dss::test::pe_raw_entry {

// The entry's name: what `/ENTRY:` (link.exe, lld-link) and `-e` (GNU ld) are handed.
inline constexpr char const* kSymbol = "dss_test_raw_entry";

// Its translation unit: one function, one import. `ExitProcess` is declared here, so the unit states that
// import and nothing else of a header.
inline constexpr char const* kSource =
    "int main(void);\n"
    "void ExitProcess(unsigned int);\n"
    "void dss_test_raw_entry(void) { ExitProcess((unsigned int)main()); }\n";

}  // namespace dss::test::pe_raw_entry
