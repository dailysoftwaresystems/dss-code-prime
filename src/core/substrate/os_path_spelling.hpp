#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

// os_path_spelling -- THE OS'S OWN SPELLING OF A PLAIN WINDOWS PATH, and the spellings a Win32 call takes made from it
// (P69 H6, D-LINK-A-DEEP-OUTPUT-DIRECTORY-FAILS-THE-WINDOWS-IMAGE-WRITE). THE ONE HOME of that rule:
// `core::extendedLengthSpelling` and `core::applicationNameSpelling` (path_identity) are built on it, and so is
// `tests/test_support/run_binary.hpp`'s own CreateProcessW.
//
// ★ WHY THE OS IS ASKED, NOT MODELLED. What a plain spelling MEANS is the OS's answer: GetFullPathNameW is the
// resolution every Win32 call applies to a non-verbatim spelling before the NT namespace sees it -- the process's
// directories for a relative one, the separators, `.` and `..` (a device spelling's climb over its volume included),
// the trailing period and space trim, a reserved DOS device name to its `\\.\NAME` -- and it is string-only: it opens
// nothing and answers at any length (✔MEASURED 2026-11-04, probe-reference-cc runs 20261104-215014-7ba4f08f and
// 20261104-215144-4893b98f, at 330 characters as at 88; its answer over F3's universe, run 20261104-222037-6ef9b98e,
// the same from a cl and a g++ binary). Which reserved names it resolves is the RELEASE's: this Windows 11 resolves a
// drive path's last `NUL` and no other, where older ones resolved `CON`, `COM1`, ... too (DOCUMENTED, "Naming Files,
// Paths, and Namespaces") -- one more reason the OS is asked. The one normalizer (`core::normalizedSpelling`), which
// also answers for a Win32 spelling on a host that has no Win32, is pinned EQUAL to it on every Windows leg
// (tests/core/test_path_identity.cpp), so the two cannot drift.
//
// ★ HEADER-ONLY AND DEPENDENCY-FREE, for windows_command_line.hpp's reason: `integrated_tests` links NO DSS library
// and reaches this through tests/test_support/run_binary.hpp's RELATIVE include, which resolves against that
// header's own directory with no include path. One project include here would break that consumer. KEEP IT SO.
//
// Windows only: no other host has the namespace these spellings belong to.

namespace dss::substrate {

#if defined(_WIN32)

// GetFullPathNameW of `plain`, or EMPTY when the OS refuses it (an empty spelling, one holding a NUL).
[[nodiscard]] inline std::wstring osFullPathSpelling(std::wstring const& plain) {
    if (plain.empty() || plain.find(L'\0') != std::wstring::npos) return {};
    DWORD const needed = ::GetFullPathNameW(plain.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return {};
    std::wstring out(needed, L'\0');
    DWORD const written = ::GetFullPathNameW(plain.c_str(), needed, out.data(), nullptr);
    if (written == 0 || written >= needed) return {};   // refused, or the directory moved between the two calls
    out.resize(written);
    return out;
}

// Whether `s` is a VERBATIM spelling -- exactly `\\?\` -- which Win32 hands to the NT namespace as written.
[[nodiscard]] inline bool isVerbatimSpelling(std::wstring const& s) {
    return s.size() >= 4 && s[0] == L'\\' && s[1] == L'\\' && s[2] == L'?' && s[3] == L'\\';
}

// THE EXTENDED-LENGTH SPELLING of what `plain` means, which a Win32 call opens at ANY length: a verbatim spelling as
// written; otherwise the OS's full spelling, prefixed -- `\\?\` before a drive, `\\?\UNC\` in place of a share's
// `\\`, `\\?\` in place of a device's `\\.\` (one NT namespace: ✔MEASURED, run 20261104-215748-2946f6a6, each opens
// the SAME object, one NT name and one file id -- a file, a trailing dot, `..`, the climb, `\\.\C:\d\NUL`, `\\.\NUL`,
// `\\.\CONIN$`, GLOBALROOT, `\\.\UNC\`, `\\.\pipe\`). A reserved device name the OS resolved to `\\.\NAME` so becomes
// `\\?\NAME`: the same device. `plain` as written when the OS refuses it.
[[nodiscard]] inline std::wstring osExtendedSpelling(std::wstring const& plain) {
    if (plain.empty() || isVerbatimSpelling(plain)) return plain;
    std::wstring const full = osFullPathSpelling(plain);
    if (full.size() >= 4 && isVerbatimSpelling(full)) return full;
    if (full.size() >= 4 && full[0] == L'\\' && full[1] == L'\\' && full[2] == L'.' && full[3] == L'\\')
        return L"\\\\?\\" + full.substr(4);
    if (full.size() >= 3 && full[0] == L'\\' && full[1] == L'\\') return L"\\\\?\\UNC\\" + full.substr(2);
    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') return L"\\\\?\\" + full;
    return plain;
}

// THE SPELLING A PROCESS'S IMAGE TAKES as CreateProcessW's application name: the extended-length spelling exactly
// where the plain one cannot start, and `plain` as written everywhere else. ✔MEASURED 2026-11-04 (probe-reference-cc
// runs 20261104-220546-3ec56792 and 20261104-220446-62443cb7, cl 19.51): a plain application name starts up to
// MAX_PATH - 1 = 259 characters and fails from 260 (ERROR_PATH_NOT_FOUND, where CreateFileW's open fails too);
// `\\?\` starts at any length, BUT the child then reads its OWN image name verbatim (GetModuleFileNameW: `\\?\...`,
// at 80 characters as at 322) -- so an image the plain spelling starts keeps the name the child reads. The length is
// the OS's full spelling's (a relative image is resolved against the caller's directory).
[[nodiscard]] inline std::wstring osApplicationName(std::wstring const& plain) {
    std::wstring const full = osFullPathSpelling(plain);
    return full.size() >= std::size_t{MAX_PATH} ? osExtendedSpelling(plain) : plain;
}

#endif  // _WIN32

}  // namespace dss::substrate
