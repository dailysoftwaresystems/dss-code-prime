#pragma once

// THE CODE OF A C++ SOURCE, as a repo guard reads it (P69 lane `cs`, fold f26d): the text with every comment and
// every string or character literal blanked to spaces, its new-lines kept -- so a line number still names the
// source line, an offset still names the same character, and prose that names a form is never read as one. Read by
// the guards that refuse a form outside its one home (test_symbol_id_mints_guard.cpp,
// test_module_symbol_id_holders.cpp, test_filesystem_queries_guard.cpp, test_no_regex_guard.cpp). Its rules are the
// ones test_filesystem_queries_guard.cpp's own copy held until P69 H6c2 deleted it (raw strings with their
// delimiters and encoding prefixes, a `'` after a digit or a letter read as a digit separator or a prefixed
// literal's quote) -- MEASURED the same over every C++ source of src/, tests/ and integrated_tests/ (1507 files,
// lane `hm`); five other tests keep a copy of their own, a neighbour stated in fold f26d's hand-back.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace dss::test {

// Whether the `R` at `i` opens a raw string literal: it begins a token, alone or after an encoding prefix.
[[nodiscard]] inline bool opensRawLiteral(std::string const& text, std::size_t i) {
    auto const ident = [&](std::size_t k) {
        unsigned char const ch = static_cast<unsigned char>(text[k]);
        return std::isalnum(ch) != 0 || ch == '_';
    };
    std::size_t start = i;
    if (start >= 2 && text.compare(start - 2, 2, "u8") == 0) start -= 2;
    else if (start >= 1 && (text[start - 1] == 'u' || text[start - 1] == 'U' || text[start - 1] == 'L')) start -= 1;
    return start == 0 || !ident(start - 1);
}

[[nodiscard]] inline std::string codeOnly(std::string const& text) {
    std::string out = text;
    std::size_t i   = 0;
    auto const  blank = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to && k < out.size(); ++k)
            if (out[k] != '\n') out[k] = ' ';
    };
    while (i < text.size()) {
        char const c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            std::size_t const end  = text.find('\n', i);
            std::size_t const stop = end == std::string::npos ? text.size() : end;
            blank(i, stop);
            i = stop;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            std::size_t const end  = text.find("*/", i + 2);
            std::size_t const stop = end == std::string::npos ? text.size() : end + 2;
            blank(i, stop);
            i = stop;
        } else if (c == 'R' && i + 1 < text.size() && text[i + 1] == '"' && opensRawLiteral(text, i)) {
            std::size_t const open = text.find('(', i + 2);
            if (open == std::string::npos) break;
            std::string const delim = ")" + text.substr(i + 2, open - (i + 2)) + "\"";
            std::size_t const end   = text.find(delim, open + 1);
            std::size_t const stop  = end == std::string::npos ? text.size() : end + delim.size();
            blank(i, stop);
            i = stop;
        } else if (c == '"' || (c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])) != 0))) {
            std::size_t k = i + 1;
            while (k < text.size() && text[k] != c && text[k] != '\n') k += text[k] == '\\' ? 2 : 1;
            std::size_t const stop = std::min(text.size(), k + 1);
            blank(i, stop);
            i = stop;
        } else {
            ++i;
        }
    }
    return out;
}

} // namespace dss::test
