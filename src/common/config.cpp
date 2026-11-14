#include "common/config.hpp"
#include "common/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace aoh {

std::string trim_copy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split_csv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto t = trim_copy(item);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

AppConfig AppConfig::make_default() {
    AppConfig cfg;
    cfg.exit_sound = {"random", ""};
    cfg.idle_sound = {"random", ""};
    cfg.random_slots = {
        "builtin://1.mp3",
        "builtin://2.mp3",
        "builtin://3.mp3",
        "builtin://4.mp3",
        "builtin://5.mp3",
    };
#ifdef _WIN32
    cfg.watch_processes = "cmd.exe,powershell.exe,pwsh.exe";
#else
    cfg.watch_processes = "bash,zsh,fish,sh";
#endif
    return cfg;
}

std::string get_config_dir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return path_to_utf8(std::filesystem::path(path) / L"A-OH");
    }
    if (const char* appdata = std::getenv("APPDATA")) {
        return (std::filesystem::path(appdata) / "A-OH").string();
    }
    return ".";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return (std::filesystem::path(home ? home : ".") / "Library" / "Application Support" / "A-OH").string();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return (std::filesystem::path(xdg) / "a-oh").string();
    }
    const char* home = std::getenv("HOME");
    return (std::filesystem::path(home ? home : ".") / ".config" / "a-oh").string();
#endif
}

std::string get_config_file_path() {
    return path_to_utf8(path_from_utf8(get_config_dir()) / "config.ini");
}

std::string get_runtime_file_path() {
    return path_to_utf8(path_from_utf8(get_config_dir()) / "runtime.ini");
}

void write_runtime_ports(int ipc_port, int web_port) {
    std::error_code ec;
    std::filesystem::create_directories(path_from_utf8(get_config_dir()), ec);
    std::ofstream ofs(path_from_utf8(get_runtime_file_path()), std::ios::binary | std::ios::trunc);
    if (ofs.is_open()) {
        ofs << "ipc_port=" << ipc_port << "\n"
            << "web_port=" << web_port << "\n";
    }
}

bool read_runtime_ports(int& ipc_port, int& web_port) {
    std::ifstream ifs(path_from_utf8(get_runtime_file_path()), std::ios::binary);
    if (!ifs.is_open()) return false;
    std::string line;
    bool got_ipc = false;
    while (std::getline(ifs, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim_copy(line.substr(0, eq));
        std::string v = trim_copy(line.substr(eq + 1));
        try {
            if (k == "ipc_port") { ipc_port = std::stoi(v); got_ipc = true; }
            else if (k == "web_port") { web_port = std::stoi(v); }
        } catch (...) {}
    }
    return got_ipc && ipc_port > 0;
}

void remove_runtime_ports() {
    std::error_code ec;
    std::filesystem::remove(path_from_utf8(get_runtime_file_path()), ec);
}

static bool parse_bool(const std::string& v, bool fallback) {
    std::string low;
    low.reserve(v.size());
    for (char c : v) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (low == "1" || low == "true" || low == "yes" || low == "on") return true;
    if (low == "0" || low == "false" || low == "no" || low == "off") return false;
    return fallback;
}

static int parse_int(const std::string& v, int fallback, int lo, int hi) {
    try {
        return std::clamp(std::stoi(v), lo, hi);
    } catch (...) {
        return fallback;
    }
}

static std::string normalize_mode(const std::string& v) {
    if (v == "mp3" || v == "random") return v;
    return "ding";
}

AppConfig load_config() {
    AppConfig cfg = AppConfig::make_default();
    std::ifstream ifs(path_from_utf8(get_config_file_path()), std::ios::binary);
    if (!ifs.is_open()) return cfg;

    int slot_count = -1;
    std::map<int, std::string> parsed_slots;

    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_copy(trimmed.substr(0, eq));
        std::string val = trim_copy(trimmed.substr(eq + 1));

        if (key == "exit_mode") cfg.exit_sound.mode = normalize_mode(val);
        else if (key == "exit_mp3_path") cfg.exit_sound.mp3_path = val;
        else if (key == "idle_mode") cfg.idle_sound.mode = normalize_mode(val);
        else if (key == "idle_mp3_path") cfg.idle_sound.mp3_path = val;
        else if (key == "slot_count") slot_count = parse_int(val, 5, 0, 100);
        else if (key.rfind("slot_", 0) == 0) {
            try {
                int idx = std::stoi(key.substr(5));
                if (idx >= 0 && idx < 100) parsed_slots[idx] = val;
            } catch (...) {}
        }
        else if (key == "volume") cfg.volume = parse_int(val, cfg.volume, 0, 100);
        else if (key == "watch_enabled") cfg.watch_enabled = parse_bool(val, cfg.watch_enabled);
        else if (key == "watch_processes") cfg.watch_processes = val;
        else if (key == "min_process_ms") cfg.min_process_ms = parse_int(val, cfg.min_process_ms, 0, 600000);
        else if (key == "poll_interval_ms") cfg.poll_interval_ms = parse_int(val, cfg.poll_interval_ms, 100, 10000);
        else if (key == "idle_seconds") cfg.idle_seconds = parse_int(val, cfg.idle_seconds, 1, 86400);
        else if (key == "ipc_port") cfg.ipc_port = parse_int(val, cfg.ipc_port, 1024, 65535);
        else if (key == "web_port") cfg.web_port = parse_int(val, cfg.web_port, 1024, 65535);
    }

    if (slot_count >= 0) {
        cfg.random_slots.assign(static_cast<size_t>(slot_count), "");
        for (const auto& kv : parsed_slots) {
            if (kv.first >= 0 && kv.first < slot_count) {
                cfg.random_slots[static_cast<size_t>(kv.first)] = kv.second;
            }
        }
    }
    return cfg;
}

bool save_config(const AppConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(path_from_utf8(get_config_dir()), ec);
    std::ofstream ofs(path_from_utf8(get_config_file_path()), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;

    ofs << "# A-OH Configuration (UTF-8)\n"
        << "exit_mode=" << cfg.exit_sound.mode << "\n"
        << "exit_mp3_path=" << cfg.exit_sound.mp3_path << "\n"
        << "idle_mode=" << cfg.idle_sound.mode << "\n"
        << "idle_mp3_path=" << cfg.idle_sound.mp3_path << "\n"
        << "slot_count=" << cfg.random_slots.size() << "\n";
    for (size_t i = 0; i < cfg.random_slots.size(); ++i) {
        ofs << "slot_" << i << "=" << cfg.random_slots[i] << "\n";
    }
    ofs << "volume=" << cfg.volume << "\n"
        << "watch_enabled=" << (cfg.watch_enabled ? "true" : "false") << "\n"
        << "watch_processes=" << cfg.watch_processes << "\n"
        << "min_process_ms=" << cfg.min_process_ms << "\n"
        << "poll_interval_ms=" << cfg.poll_interval_ms << "\n"
        << "idle_seconds=" << cfg.idle_seconds << "\n"
        << "ipc_port=" << cfg.ipc_port << "\n"
        << "web_port=" << cfg.web_port << "\n";
    return ofs.good();
}

} // namespace aoh
