#pragma once

#include <cctype>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace aoh {

inline bool detect_system_is_zh() {
#ifdef _WIN32
    LANGID lid = GetUserDefaultUILanguage();
    return PRIMARYLANGID(lid) == LANG_CHINESE;
#else
    for (const char* env_name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* val = std::getenv(env_name);
        if (val && *val) {
            std::string s(val);
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s.rfind("zh", 0) == 0;
        }
    }
    return false;
#endif
}

inline std::string normalize_language(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (s == "zh" || s == "zh-cn" || s == "zh_cn" || s == "cn") return "zh";
    if (s == "en" || s == "en-us" || s == "en_us") return "en";
    return "auto";
}

inline bool is_zh_language(const std::string& cfg_lang) {
    std::string norm = normalize_language(cfg_lang);
    if (norm == "zh") return true;
    if (norm == "en") return false;
    return detect_system_is_zh();
}

} // namespace aoh
