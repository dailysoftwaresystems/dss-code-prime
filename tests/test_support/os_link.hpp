#pragma once

// A LINK MADE BY THE OS -- the one way a test makes a symbolic link or a junction.
//
// ★★★ WHY NOT `std::filesystem::create_symlink` / `create_directory_symlink`. ✔MEASURED 2026-10-20,
// one probe built by both libraries that build DSS on Windows: libstdc++ 13.2 (MinGW-w64 UCRT) fails
// both with "Function not implemented" -- no privilege changes that, it never asks the OS -- while
// MS STL, and the OS's own `CreateSymbolicLinkW`, make the same link in the same session. A case that
// made its link through the library therefore SKIPPED, or failed, on the MinGW leg -- the one leg
// whose library cannot SEE a link either (`symlink_status` there reads what the link reaches), so
// the defect the case guarded went unmeasured exactly where it lived. Here the link is made by the
// OS on Windows (`SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`: a host in developer mode needs no
// elevation) and by the library elsewhere, which makes it there; what a link IS, a test asks
// `dss::core::entryKind`, never the library.
//
// A JUNCTION exists only on Windows; it is made by the OS's own `mklink /J`, and asking for one
// elsewhere is an error naming why.

#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <cstdlib>   // _wsystem
#endif

namespace dss::test_support {

// What a symbolic link is made to name: a directory or a file. (Windows records which on the link
// itself; a link to a target that does not exist yet must still say which it is.)
enum class LinkTo { Directory, File };

// Make the symbolic link `link` naming `target`. Empty on success, else why the OS refused.
[[nodiscard]] inline std::error_code makeSymbolicLink(std::filesystem::path const& target,
                                                      std::filesystem::path const& link, LinkTo to) {
#if defined(_WIN32)
    DWORD const flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
                      | (to == LinkTo::Directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (::CreateSymbolicLinkW(link.c_str(), target.c_str(), flags) == 0)
        return std::error_code{static_cast<int>(::GetLastError()), std::system_category()};
    return {};
#else
    std::error_code ec;
    if (to == LinkTo::Directory)
        std::filesystem::create_directory_symlink(target, link, ec);
    else
        std::filesystem::create_symlink(target, link, ec);
    return ec;
#endif
}

// Make the junction `link` naming the directory `target` (Windows only). Empty on success; else
// why, `function_not_supported` off Windows.
[[nodiscard]] inline std::error_code makeJunction(std::filesystem::path const& target,
                                                  std::filesystem::path const& link) {
#if defined(_WIN32)
    std::wstring const command = L"cmd /d /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring()
                               + L"\" >nul 2>&1";
    int const rc = ::_wsystem(command.c_str());
    DWORD const attributes = ::GetFileAttributesW(link.c_str());
    if (rc != 0 || attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        return std::make_error_code(std::errc::operation_not_permitted);
    return {};
#else
    (void)target;
    (void)link;
    return std::make_error_code(std::errc::function_not_supported);
#endif
}

} // namespace dss::test_support
