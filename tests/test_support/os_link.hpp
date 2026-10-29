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
// A JUNCTION exists only on Windows; it is made by the OS's own call (a mount-point reparse point set
// on an empty directory, which needs no privilege), and asking for one elsewhere is an error naming why.
//
// ★ A REFUSAL SAYS WHAT THE HOST LACKS. A Windows host neither in developer mode nor elevated refuses
// every symbolic link (ERROR_PRIVILEGE_NOT_HELD), and every case that makes one is then red: the error a
// helper returns is in this header's category, whose message is the OS's own and, for that refusal,
// names the setting that grants it. A case asserts its link with `DSS_ASSERT_LINK_MADE(call)`, which
// prints that message -- a red that says "developer mode" instead of a bare `true`.

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
  #include <winioctl.h>   // FSCTL_SET_REPARSE_POINT
  #include <cstring>
  #include <vector>
#endif

namespace dss::test_support {

// The category of a link the OS refused: the OS's own code and message, and, for the refusal a host without
// the privilege gives, what grants it. It compares equal to the OS's own codes (`default_error_condition`).
class LinkRefusalCategory final : public std::error_category {
public:
    [[nodiscard]] char const* name() const noexcept override { return "os-link"; }
    [[nodiscard]] std::string message(int code) const override {
        std::string text = std::system_category().message(code);
#if defined(_WIN32)
        if (code == ERROR_PRIVILEGE_NOT_HELD) {
            text += " -- this host may not make a symbolic link: turn on Windows developer mode (Settings > System "
                    "> For developers), or run the tests elevated";
        }
#endif
        return text;
    }
    [[nodiscard]] std::error_condition default_error_condition(int code) const noexcept override {
        return std::system_category().default_error_condition(code);
    }
};

[[nodiscard]] inline std::error_category const& linkRefusalCategory() {
    static LinkRefusalCategory const category;
    return category;
}

// What a link is made to name: a directory or a file. (Windows records which on the link
// itself; a link to a target that does not exist yet must still say which it is.)
enum class LinkTo { Directory, File };

// Make the symbolic link `link` naming `target`. Empty on success, else why the OS refused.
[[nodiscard]] inline std::error_code makeSymbolicLink(std::filesystem::path const& target,
                                                      std::filesystem::path const& link, LinkTo to) {
#if defined(_WIN32)
    DWORD const flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
                      | (to == LinkTo::Directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (::CreateSymbolicLinkW(link.c_str(), target.c_str(), flags) == 0)
        return std::error_code{static_cast<int>(::GetLastError()), linkRefusalCategory()};
    return {};
#else
    std::error_code ec;
    if (to == LinkTo::Directory)
        std::filesystem::create_directory_symlink(target, link, ec);
    else
        std::filesystem::create_symlink(target, link, ec);
    return ec ? std::error_code{ec.value(), linkRefusalCategory()} : std::error_code{};
#endif
}

// Make the junction `link` naming the directory `target` (Windows only): an empty directory with a
// mount-point reparse point naming `target` absolutely. Empty on success; else the OS's reason,
// `function_not_supported` off Windows.
[[nodiscard]] inline std::error_code makeJunction(std::filesystem::path const& target,
                                                  std::filesystem::path const& link) {
#if defined(_WIN32)
    auto const refused = [](DWORD error) { return std::error_code{static_cast<int>(error), linkRefusalCategory()}; };
    std::error_code    absEc;
    std::wstring const print = std::filesystem::absolute(target, absEc).wstring();
    if (absEc) return absEc;
    std::wstring const substitute = L"\\??\\" + print;
    if (::CreateDirectoryW(link.c_str(), nullptr) == 0) return refused(::GetLastError());
    HANDLE const dir = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        DWORD const error = ::GetLastError();
        ::RemoveDirectoryW(link.c_str());
        return refused(error);
    }
    // The mount-point layout of REPARSE_DATA_BUFFER (ntifs.h, not in the user-mode headers): the tag, the data
    // length, a reserved word, then the two names' offsets and lengths and the names themselves, each ended by
    // a NUL the lengths do not count.
    USHORT const substituteBytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    USHORT const printBytes      = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    USHORT const pathBytes       = static_cast<USHORT>(substituteBytes + printBytes + 2 * sizeof(wchar_t));
    USHORT const dataBytes       = static_cast<USHORT>(4 * sizeof(USHORT) + pathBytes);
    std::vector<unsigned char> buffer(8 + dataBytes, 0);
    auto const put = [&](std::size_t at, auto value) { std::memcpy(buffer.data() + at, &value, sizeof value); };
    put(0, static_cast<ULONG>(IO_REPARSE_TAG_MOUNT_POINT));
    put(4, dataBytes);
    put(8, static_cast<USHORT>(0));                                       // SubstituteNameOffset
    put(10, substituteBytes);                                             // SubstituteNameLength
    put(12, static_cast<USHORT>(substituteBytes + sizeof(wchar_t)));      // PrintNameOffset
    put(14, printBytes);                                                  // PrintNameLength
    std::memcpy(buffer.data() + 16, substitute.data(), substituteBytes);
    std::memcpy(buffer.data() + 16 + substituteBytes + sizeof(wchar_t), print.data(), printBytes);
    DWORD      returned = 0;
    BOOL const set      = ::DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                            static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    DWORD const error = set ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(dir);
    if (!set) {
        ::RemoveDirectoryW(link.c_str());
        return refused(error);
    }
    return {};
#else
    (void)target;
    (void)link;
    return std::make_error_code(std::errc::function_not_supported);
#endif
}

} // namespace dss::test_support

// THE ONE ASSERTION that a case's link was made (gtest): when the OS refused, the case FAILS -- it never skips --
// printing why, which names the privilege a host lacks. Used inside a TEST body (it returns from it).
#define DSS_ASSERT_LINK_MADE(call)                                                                                 \
    do {                                                                                                           \
        std::error_code const dss_link_refusal_ = (call);                                                          \
        ASSERT_FALSE(dss_link_refusal_) << "the OS made no link here, so this case cannot ask its question: "     \
                                        << dss_link_refusal_.message();                                            \
    } while (false)
