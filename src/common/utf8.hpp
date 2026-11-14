#pragma once

#include <string>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace aoh {

#ifdef _WIN32
inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

inline std::filesystem::path path_from_utf8(const std::string& s) {
    return std::filesystem::path(utf8_to_wide(s));
}

inline std::string path_to_utf8(const std::filesystem::path& p) {
    return wide_to_utf8(p.wstring());
}
#else
inline std::filesystem::path path_from_utf8(const std::string& s) {
    return std::filesystem::path(s);
}

inline std::string path_to_utf8(const std::filesystem::path& p) {
    return p.string();
}
#endif

} // namespace aoh
