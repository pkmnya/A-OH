#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aoh {

struct ProcessInfo {
    uint64_t pid = 0;
    std::string name;         // Lowercase executable basename (e.g. "cmd.exe", "bash")
    uint64_t start_key = 0;   // Unique process start timestamp / key to handle PID reuse
    uint64_t age_ms = 0;      // Approximate process lifetime in ms (0 if unknown)
};

struct TrayCallbacks {
    std::function<void()> on_open_settings;
    std::function<void()> on_pick_exit_mp3;
    std::function<void()> on_pick_idle_mp3;
    std::function<void()> on_test_exit;
    std::function<void()> on_test_idle;
    std::function<bool()> is_watch_enabled;
    std::function<void()> on_toggle_watch;
    std::function<bool()> is_autostart_enabled;
    std::function<void()> on_toggle_autostart;
    std::function<bool()> is_zh;
    std::function<void()> on_quit;
};

void net_init();
void net_cleanup();

std::string get_executable_path();
std::string get_daemon_autostart_path();
std::vector<ProcessInfo> list_processes();

bool is_autostart_enabled();
bool set_autostart(bool enable, std::string& err);

bool open_url_in_browser(const std::string& url);
std::string pick_file_dialog(const std::string& title_utf8, const std::string& kind); // "mp3" or "midi"

// Runs OS tray / signal wait loop until quit requested
void run_tray_or_wait_loop(const TrayCallbacks& cb);
void request_tray_quit();

} // namespace aoh
