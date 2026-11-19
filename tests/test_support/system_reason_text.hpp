#pragma once

// THE SYSTEM'S OWN TEXT FOR AN OS CODE -- the test side's ONE oracle for the reason class.
//
// A host primitive that fails answers the OS's reason, read at the failing call, and every diagnostic built on it
// names that reason through the product's one rendering, `core::reasonText`: `GetLastError=<n>: <text>` (Windows) or
// `errno=<n>: <text>` (POSIX). A cell that checks such a diagnostic asks the HOST the same question itself -- the same
// call on the same spelling -- and compares the product's words with `systemReasonText(<the host's code>)`, built HERE
// independently of `core::reasonText` (FormatMessageW spelt as UTF-8 by this header's own conversion; `std::strerror`
// on POSIX). Asking the product for its own expected text would compare it with itself.

#include <string>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <cstring>
#endif

namespace dss::test_support {

#if defined(_WIN32)
[[nodiscard]] inline std::string systemReasonText(DWORD code) {
    wchar_t*    buffer = nullptr;
    DWORD const length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring wide = length != 0 && buffer != nullptr ? std::wstring{buffer, length} : std::wstring{};
    if (buffer != nullptr) ::LocalFree(buffer);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) wide.pop_back();
    std::string text;
    if (!wide.empty()) {
        int const n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                            nullptr);
        text.resize(static_cast<std::size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), n, nullptr, nullptr);
    }
    return "GetLastError=" + std::to_string(code) + ": " + (text.empty() ? "<no message text for this code>" : text);
}
#else
[[nodiscard]] inline std::string systemReasonText(int code) {
    return "errno=" + std::to_string(code) + ": " + std::strerror(code);
}
#endif

}  // namespace dss::test_support
