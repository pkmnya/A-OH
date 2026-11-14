#pragma once

#include <string>
#include <vector>

namespace aoh {

struct SoundSlotConfig {
    std::string mode = "ding"; // "ding" | "mp3" | "random"
    std::string mp3_path;
};

struct AppConfig {
    SoundSlotConfig exit_sound{"ding", ""};
    SoundSlotConfig idle_sound{"ding", ""};
    std::vector<std::string> random_slots; // Default 5 slots; user can add/remove freely

    int volume = 85;             // 0 - 100
    bool watch_enabled = true;   // Watch terminal/batch process exit globally
    std::string watch_processes; // Comma-separated process names
    std::string language = "auto"; // "auto" | "zh" | "en"
    int min_process_ms = 1200;   // Ignore short-lived sub-shells
    int poll_interval_ms = 350;  // Process snapshot interval
    int idle_seconds = 10;       // Default idle threshold for `watch` / hooks
    int ipc_port = 54789;
    int web_port = 54790;

    static AppConfig make_default();
};

std::string get_config_dir();
std::string get_config_file_path();
std::string get_runtime_file_path();

void write_runtime_ports(int ipc_port, int web_port);
bool read_runtime_ports(int& ipc_port, int& web_port);
void remove_runtime_ports();

AppConfig load_config();
bool save_config(const AppConfig& cfg);
std::vector<std::string> split_csv(const std::string& csv);
std::string trim_copy(const std::string& s);

} // namespace aoh
