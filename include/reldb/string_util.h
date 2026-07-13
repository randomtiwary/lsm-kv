#pragma once

// Small string helpers shared by SQL server, shell, and tests.

#include <cctype>
#include <string>
#include <string_view>

namespace reldb {

// Trim leading/trailing ASCII space, tab, and CR (not newline — line-oriented
// protocols often keep structure via '\n' elsewhere).
inline std::string_view TrimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Also trim newlines (useful for whole-buffer / interactive shell lines).
inline std::string_view TrimViewWs(std::string_view s) {
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

// ASCII uppercase copy (for case-insensitive command matching).
inline std::string ToUpperAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out;
}

}  // namespace reldb
