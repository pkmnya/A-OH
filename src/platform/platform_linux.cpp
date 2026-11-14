#include "platform/platform.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace aoh {

namespace {

std::atomic<bool> g_quit_flag{false};

void posix_sig_handler(int) {
    g_quit_flag.store(true, std::memory_order_release);
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string desktop_autostart_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base = (xdg && *xdg)
        ? std::filesystem::path(xdg)
        : (std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config");
    return (base / "autostart" / "a-oh.desktop").string();
}

uint64_t read_uptime_ticks(long clk_tck) {
    std::ifstream ifs("/proc/uptime");
    double up = 0.0;
    if (ifs >> up) {
        return static_cast<uint64_t>(up * static_cast<double>(clk_tck));
    }
    return 0;
}

} // namespace

void net_init() {
    signal(SIGPIPE, SIG_IGN);
}

void net_cleanup() {}

std::string get_executable_path() {
    char buf[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        return std::string(buf, static_cast<size_t>(len));
    }
    return "a-oh";
}

std::string get_daemon_autostart_path() {
    return get_executable_path();
}

std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> out;
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    uint64_t up_ticks = read_uptime_ticks(clk_tck);

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        if (!entry.is_directory()) continue;
        std::string fname = entry.path().filename().string();
        if (fname.empty() || !std::all_of(fname.begin(), fname.end(), [](unsigned char c) { return std::isdigit(c); })) {
            continue;
        }
        uint64_t pid = 0;
        try {
            pid = std::stoull(fname);
        } catch (...) {
            continue;
        }

        std::ifstream stat_ifs(entry.path() / "stat");
        std::string stat_line;
        if (!std::getline(stat_ifs, stat_line)) continue;
        auto lpar = stat_line.find('(');
        auto rpar = stat_line.rfind(')');
        if (lpar == std::string::npos || rpar == std::string::npos || rpar <= lpar) continue;

        std::string comm = stat_line.substr(lpar + 1, rpar - lpar - 1);
        std::istringstream iss(stat_line.substr(rpar + 2));
        std::string tok;
        uint64_t start_ticks = 0;
        // Field 3 is state, field 22 is starttime (so skip 19 tokens after ')')
        for (int i = 3; i <= 22; ++i) {
            if (!(iss >> tok)) break;
            if (i == 22) {
                try {
                    start_ticks = std::stoull(tok);
                } catch (...) {}
            }
        }

        ProcessInfo info;
        info.pid = pid;
        info.name = to_lower_ascii(comm);
        info.start_key = start_ticks;
        if (up_ticks >= start_ticks) {
            info.age_ms = ((up_ticks - start_ticks) * 1000ULL) / static_cast<uint64_t>(clk_tck);
        }
        out.push_back(std::move(info));
    }
    return out;
}

bool is_autostart_enabled() {
    std::error_code ec;
    return std::filesystem::exists(desktop_autostart_path(), ec);
}

bool set_autostart(bool enable, std::string& err) {
    auto p = std::filesystem::path(desktop_autostart_path());
    std::error_code ec;
    if (!enable) {
        std::filesystem::remove(p, ec);
        if (ec) {
            err = "Failed to remove autostart .desktop: " + ec.message();
            return false;
        }
        return true;
    }

    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::trunc);
    if (!ofs.is_open()) {
        err = "Failed to write autostart .desktop";
        return false;
    }
    std::string exe = get_daemon_autostart_path();
    ofs << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=A-OH Terminal Sound Daemon\n"
        << "Exec=\"" << exe << "\" daemon\n"
        << "Hidden=false\n"
        << "NoDisplay=true\n"
        << "X-GNOME-Autostart-enabled=true\n";
    return ofs.good();
}

bool open_url_in_browser(const std::string& url) {
    std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
}

std::string pick_file_dialog(const std::string& title_utf8, const std::string& kind) {
    std::string filter = (kind == "midi") ? "*.mid *.midi" : "*.mp3 *.wav *.m4a";
    std::string cmd = "zenity --file-selection --title=\"" + title_utf8 +
                      "\" --file-filter=\"" + filter + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return {};
    char buf[4096] = {};
    std::string res;
    while (fgets(buf, sizeof(buf), fp)) res += buf;
    pclose(fp);
    while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) res.pop_back();
    return res;
}

void run_tray_or_wait_loop(const TrayCallbacks& cb) {
    g_quit_flag.store(false, std::memory_order_release);
    signal(SIGINT, posix_sig_handler);
    signal(SIGTERM, posix_sig_handler);
    while (!g_quit_flag.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (cb.on_quit) cb.on_quit();
}

void request_tray_quit() {
    g_quit_flag.store(true, std::memory_order_release);
}

} // namespace aoh
