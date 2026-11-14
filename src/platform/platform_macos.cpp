#include "platform/platform.hpp"

#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

std::string plist_path() {
    const char* home = std::getenv("HOME");
    return (std::filesystem::path(home ? home : ".") / "Library" / "LaunchAgents" / "com.aoh.daemon.plist").string();
}

} // namespace

void net_init() {
    signal(SIGPIPE, SIG_IGN);
}

void net_cleanup() {}

std::string get_executable_path() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        if (!buf.empty() && buf.back() == '\0') buf.pop_back();
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(buf, ec);
        return ec ? buf : canon.string();
    }
    return "a-oh";
}

std::string get_daemon_autostart_path() {
    return get_executable_path();
}

std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> out;
    int bytes = proc_listallpids(nullptr, 0);
    if (bytes <= 0) return out;
    std::vector<pid_t> pids(static_cast<size_t>(bytes) / sizeof(pid_t) + 64);
    int actual = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (actual <= 0) return out;
    int count = actual / static_cast<int>(sizeof(pid_t));

    struct timeval now_tv = {};
    gettimeofday(&now_tv, nullptr);
    uint64_t now_ms = static_cast<uint64_t>(now_tv.tv_sec) * 1000ULL + static_cast<uint64_t>(now_tv.tv_usec / 1000);

    for (int i = 0; i < count; ++i) {
        pid_t pid = pids[i];
        if (pid <= 0) continue;
        struct proc_bsdinfo bsd = {};
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd)) <= 0) continue;

        ProcessInfo info;
        info.pid = static_cast<uint64_t>(pid);
        info.name = to_lower_ascii(bsd.pbi_comm);
        uint64_t start_ms = static_cast<uint64_t>(bsd.pbi_start_tvsec) * 1000ULL +
                            static_cast<uint64_t>(bsd.pbi_start_tvusec / 1000);
        info.start_key = start_ms;
        info.age_ms = (now_ms >= start_ms) ? (now_ms - start_ms) : 0;
        out.push_back(std::move(info));
    }
    return out;
}

bool is_autostart_enabled() {
    std::error_code ec;
    return std::filesystem::exists(plist_path(), ec);
}

bool set_autostart(bool enable, std::string& err) {
    auto p = std::filesystem::path(plist_path());
    std::error_code ec;
    if (!enable) {
        std::filesystem::remove(p, ec);
        if (ec) {
            err = "Failed to remove LaunchAgent plist: " + ec.message();
            return false;
        }
        return true;
    }

    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::trunc);
    if (!ofs.is_open()) {
        err = "Failed to write LaunchAgent plist";
        return false;
    }
    std::string exe = get_daemon_autostart_path();
    ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        << "<plist version=\"1.0\">\n<dict>\n"
        << "  <key>Label</key><string>com.aoh.daemon</string>\n"
        << "  <key>ProgramArguments</key>\n  <array>\n"
        << "    <string>" << exe << "</string>\n"
        << "    <string>daemon</string>\n"
        << "  </array>\n"
        << "  <key>RunAtLoad</key><true/>\n"
        << "  <key>KeepAlive</key><false/>\n"
        << "</dict>\n</plist>\n";
    return ofs.good();
}

bool open_url_in_browser(const std::string& url) {
    std::string cmd = "open '" + url + "' >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
}

std::string pick_file_dialog(const std::string& title_utf8, const std::string& kind) {
    std::string ext = (kind == "midi") ? "{\"mid\",\"midi\"}" : "{\"mp3\",\"wav\",\"m4a\"}";
    std::string cmd = "osascript -e 'POSIX path of (choose file of type " + ext +
                      " with prompt \"" + title_utf8 + "\")' 2>/dev/null";
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
