#pragma once
#include <string>

inline std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'\"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
