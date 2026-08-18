#include "core/substrate/path_identity.hpp"

#include <system_error>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace dss::core {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32

// `GetLongPathNameW` for ONE path, or empty when Windows will not answer.
//
// ★ WHY THIS CALL AND NOT `GetFinalPathNameByHandleW`. Both expand 8.3, but the
// handle-based one ALSO rewrites junctions and mount points and prefixes `\\?\`.
// These keys are compared as strings and some of them are printed to operators,
// so the minimal transformation is the correct one: expand short components,
// touch nothing else.
[[nodiscard]] std::wstring longNameOrEmpty(std::wstring const& in) {
    if (in.empty()) return {};
    DWORD const n = ::GetLongPathNameW(in.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    DWORD const written = ::GetLongPathNameW(in.c_str(), out.data(), n);
    // `written >= n` means the path changed under us between the two calls.
    if (written == 0 || written >= n) return {};
    out.resize(written);
    return out;
}

// Expand the longest EXISTING prefix and re-attach the remainder verbatim.
//
// ★★★ THIS WALK IS MANDATORY, NOT DEFENSIVE — ✔MEASURED 2026-08-18 on this
// machine, with a standalone probe, before this function was written:
//
//     DSS-SC~1                   ->  dss-scratch-spelling-probe-longname   (expanded)
//     DSS-SC~1\not\yet\built.o   ->  DSS-SC~1\not\yet\built.o              (UNCHANGED)
//
// `GetLongPathNameW` fails WHOLESALE when any component is absent. Calling it on
// the full path alone therefore no-ops on exactly the paths that do not exist
// yet — every output artifact, every not-yet-generated source — while looking
// perfectly correct on every input path that happens to exist. That is a false
// green of the family this project keeps catching: the property appears to hold
// because the cases that would refute it were never exercised.
[[nodiscard]] std::wstring expandShortComponents(std::wstring const& in) {
    if (auto const whole = longNameOrEmpty(in); !whole.empty()) return whole;

    fs::path const p{in};
    fs::path       head = p;
    fs::path       tail;
    // Walk up until Windows answers for the head. `parent_path()` of a root is
    // the root itself, so compare against the previous value to terminate —
    // `has_parent_path()` alone would spin forever on `C:\`.
    while (true) {
        fs::path const parent = head.parent_path();
        if (parent.empty() || parent == head) return in;  // nothing existed
        tail = head.filename() / tail;
        head = parent;
        if (auto const expanded = longNameOrEmpty(head.wstring());
            !expanded.empty()) {
            fs::path out{expanded};
            out /= tail;
            return out.wstring();
        }
    }
}

#endif  // _WIN32

}  // namespace

std::string canonicalIdentityKey(fs::path const& p) {
    std::error_code ec;
    return canonicalIdentityKey(p, ec);
}

std::string canonicalIdentityKey(fs::path const& p, std::error_code& ec) {
    ec.clear();
    fs::path const c = fs::weakly_canonical(p, ec);
    fs::path const r = ec ? p.lexically_normal() : c;
#ifdef _WIN32
    // `generic_string()` and not `string()`: ONE separator convention for every
    // key in the compiler. Two sites that key the same file with different
    // separators disagree about identity for a reason no diagnostic can explain,
    // and the sites this replaced were split between the two spellings.
    return fs::path{expandShortComponents(r.wstring())}.generic_string();
#else
    return r.generic_string();
#endif
}

}  // namespace dss::core
