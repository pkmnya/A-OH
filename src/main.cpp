#include "audio/audio_engine.hpp"
#include "common/config.hpp"
#include "common/utf8.hpp"
#include "daemon/daemon.hpp"
#include "platform/platform.hpp"
#include "watch/watcher.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

void print_help() {
    std::cout
        << "A-OH - 跨平台终端/批处理退出 & 闲置音效助手 (支持 MP3 常驻内存、打断不重叠)\n\n"
        << "用法:\n"
        << "  a-oh daemon [--ui]            启动后台监听守护进程 (Windows 带托盘图标)\n"
        << "  a-oh ui                       打开音效设置网页 (若未运行后台则自动启动)\n"
        << "  a-oh play [exit|idle]         触发一次音效 (通知后台打断当前音并播放)\n"
        << "  a-oh stop                     立即打断静音\n"
        << "  a-oh watch [--idle N] -- cmd  包装运行命令/批处理: 输出闲置 N 秒响 idle, 结束响 exit\n"
        << "  a-oh config show              查看当前配置与路径\n"
        << "  a-oh config set-mp3 <exit|idle> <path>  设置单个 MP3 并常驻内存\n"
        << "  a-oh config use-random <exit|idle>      开启从随机槽位池抽取播放\n"
        << "  a-oh config use-ding <exit|idle>        恢复内置清脆滴滴声\n"
        << "  a-oh config set-slot <1..N> <path>      设置第 N 个随机槽位的 MP3\n"
        << "  a-oh config add-slot [path]             新增一个随机槽位\n"
        << "  a-oh config remove-slot [1..N]          删除指定(或最后)随机槽位\n"
        << "  a-oh config volume <0-100>              设置全局音量\n"
        << "  a-oh autostart <on|off|status>          开启/关闭/查看开机自启动\n"
        << "  a-oh hook <pwsh|bash|zsh>               生成终端闲置/退出 Shell 钩子脚本\n"
        << "  a-oh status                   查看后台守护进程与内存缓存状态\n"
        << "  a-oh quit                     退出后台守护进程\n";
}

int dispatch(const std::vector<std::string>& args) {
    if (args.empty()) {
#ifdef AOH_GUI_APP
        return aoh::run_daemon(false);
#else
        print_help();
        return 0;
#endif
    }

    const std::string& cmd = args[0];
    aoh::AppConfig cfg = aoh::load_config();

    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        print_help();
        return 0;
    }

    if (cmd == "daemon") {
        bool open_ui = (args.size() >= 2 && args[1] == "--ui");
        return aoh::run_daemon(open_ui);
    }

    if (cmd == "ui") {
        std::string pong;
        if (aoh::send_ipc_command(cfg.ipc_port, "PING", pong) && pong == "PONG") {
            int ipc_p = cfg.ipc_port, web_p = cfg.web_port;
            aoh::read_runtime_ports(ipc_p, web_p);
            aoh::open_url_in_browser("http://127.0.0.1:" + std::to_string(web_p));
            return 0;
        }
        return aoh::run_daemon(true);
    }

    if (cmd == "play") {
        std::string which = (args.size() >= 2) ? args[1] : "exit";
        std::string reply;
        std::string ipc = (which == "idle") ? "PLAY_IDLE" : "PLAY_EXIT";
        if (aoh::send_ipc_command(cfg.ipc_port, ipc, reply)) {
            return 0;
        }
        aoh::AudioEngine eng;
        if (!eng.init()) return 1;
        if (which == "idle") eng.play_idle(cfg);
        else eng.play_exit(cfg);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        eng.shutdown();
        return 0;
    }

    if (cmd == "stop") {
        std::string reply;
        aoh::send_ipc_command(cfg.ipc_port, "STOP", reply);
        return 0;
    }

    if (cmd == "quit") {
        std::string reply;
        if (aoh::send_ipc_command(cfg.ipc_port, "QUIT", reply)) {
            std::cout << "Daemon stopped.\n";
            return 0;
        }
        std::cout << "Daemon is not running.\n";
        return 0;
    }

    if (cmd == "status") {
        std::string reply;
        if (aoh::send_ipc_command(cfg.ipc_port, "STATUS", reply)) {
            std::cout << reply << "\n";
            return 0;
        }
        std::cout << "Daemon not running. Config file: " << aoh::get_config_file_path() << "\n";
        return 1;
    }

    if (cmd == "autostart") {
        std::string sub = (args.size() >= 2) ? args[1] : "status";
        if (sub == "status") {
            std::cout << "Autostart: " << (aoh::is_autostart_enabled() ? "ON" : "OFF") << "\n";
            return 0;
        }
        std::string err;
        bool enable = (sub == "on" || sub == "enable" || sub == "1");
        if (!aoh::set_autostart(enable, err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
        std::cout << "Autostart: " << (enable ? "ON" : "OFF") << "\n";
        return 0;
    }

    if (cmd == "config") {
        std::string sub = (args.size() >= 2) ? args[1] : "show";
        if (sub == "show") {
            std::cout << "Config path: " << aoh::get_config_file_path() << "\n"
                      << "exit_mode: " << cfg.exit_sound.mode << "\n"
                      << "exit_mp3_path: " << cfg.exit_sound.mp3_path << "\n"
                      << "idle_mode: " << cfg.idle_sound.mode << "\n"
                      << "idle_mp3_path: " << cfg.idle_sound.mp3_path << "\n"
                      << "slots (" << cfg.random_slots.size() << "):\n";
            for (size_t i = 0; i < cfg.random_slots.size(); ++i) {
                std::cout << "  [" << (i + 1) << "] " << cfg.random_slots[i] << "\n";
            }
            std::cout << "volume: " << cfg.volume << "\n"
                      << "watch_enabled: " << (cfg.watch_enabled ? "true" : "false") << "\n"
                      << "watch_processes: " << cfg.watch_processes << "\n"
                      << "min_process_ms: " << cfg.min_process_ms << "\n"
                      << "idle_seconds: " << cfg.idle_seconds << "\n"
                      << "autostart: " << (aoh::is_autostart_enabled() ? "ON" : "OFF") << "\n";
            return 0;
        }
        if (sub == "use-random" && args.size() >= 3) {
            const std::string& slot = args[2];
            if (slot == "idle") cfg.idle_sound.mode = "random";
            else cfg.exit_sound.mode = "random";
            aoh::save_config(cfg);
            std::string reply;
            aoh::send_ipc_command(cfg.ipc_port, "RELOAD", reply);
            std::cout << "Updated " << slot << " -> random slot pool\n";
            return 0;
        }
        if (sub == "set-slot" && args.size() >= 4) {
            int idx1 = std::stoi(args[2]);
            if (idx1 < 1 || idx1 > 100) {
                std::cerr << "Slot index must be 1..100\n";
                return 1;
            }
            if (static_cast<size_t>(idx1) > cfg.random_slots.size()) {
                cfg.random_slots.resize(static_cast<size_t>(idx1));
            }
            cfg.random_slots[static_cast<size_t>(idx1 - 1)] = args[3];
            aoh::save_config(cfg);
            std::string reply;
            aoh::send_ipc_command(cfg.ipc_port, "RELOAD", reply);
            std::cout << "Updated slot #" << idx1 << " -> " << args[3] << "\n";
            return 0;
        }
        if (sub == "add-slot") {
            std::string path = (args.size() >= 3) ? args[2] : "";
            cfg.random_slots.push_back(path);
            aoh::save_config(cfg);
            std::string reply;
            aoh::send_ipc_command(cfg.ipc_port, "RELOAD", reply);
            std::cout << "Added slot #" << cfg.random_slots.size() << "\n";
            return 0;
        }
        if (sub == "remove-slot") {
            if (cfg.random_slots.size() <= 1) {
                cfg.random_slots.assign(1, "");
            } else if (args.size() >= 3) {
                int idx1 = std::stoi(args[2]);
                if (idx1 >= 1 && static_cast<size_t>(idx1) <= cfg.random_slots.size()) {
                    cfg.random_slots.erase(cfg.random_slots.begin() + (idx1 - 1));
                }
            } else {
                cfg.random_slots.pop_back();
            }
            aoh::save_config(cfg);
            std::string reply;
            aoh::send_ipc_command(cfg.ipc_port, "RELOAD", reply);
            std::cout << "Remaining slots: " << cfg.random_slots.size() << "\n";
            return 0;
        }
        if (sub == "set-mp3" && args.size() >= 4) {
            const std::string& slot = args[2];
            const std::string& path = args[3];
            if (slot == "idle") {
                cfg.idle_sound.mode = "mp3";
                cfg.idle_sound.mp3_path = path;
            } else {
                cfg.exit_sound.mode = "mp3";
                cfg.exit_sound.mp3_path = path;
            }
            aoh::save_config(cfg);
            std::string reply;
            aoh::send_ipc_command(cfg.ipc_port, "RELOAD", reply);
            std::cout << "Updated " << slot << " MP3 -> " << path << "\n";
            return 0;
        }
        if (sub == "use-ding" && args.size() >= 3) {
            const std::string& slot = args[2];
            if (slot == "idle") cfg.idle_sound.mode = "ding";
            else cfg.exit_sound.mode = "ding";
            aoh::save_config(cfg);
            std::string reply;
            aoh::send_ipc_command(cfg.ipc_port, "RELOAD", reply);
            std::cout << "Updated " << slot << " -> built-in ding\n";
            return 0;
        }
        if (sub == "volume" && args.size() >= 3) {
            cfg.volume = std::clamp(std::stoi(args[2]), 0, 100);
            aoh::save_config(cfg);
            std::string reply;
            aoh::send_ipc_command(cfg.ipc_port, "RELOAD", reply);
            std::cout << "Volume set to " << cfg.volume << "\n";
            return 0;
        }
        print_help();
        return 1;
    }

    if (cmd == "watch") {
        int idle_s = 0;
        std::vector<std::string> cmd_args;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--idle" && i + 1 < args.size()) {
                idle_s = std::stoi(args[++i]);
            } else if (args[i] == "--") {
                for (size_t j = i + 1; j < args.size(); ++j) cmd_args.push_back(args[j]);
                break;
            } else {
                for (size_t j = i; j < args.size(); ++j) cmd_args.push_back(args[j]);
                break;
            }
        }
        return aoh::run_watch_command(idle_s, cmd_args);
    }

    if (cmd == "hook") {
        std::string sh = (args.size() >= 2) ? args[1] : "pwsh";
        aoh::print_shell_hook(sh);
        return 0;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    print_help();
    return 1;
}

} // namespace

#ifdef _WIN32
#ifdef AOH_GUI_APP
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    aoh::net_init();
    std::vector<std::string> args;
    if (wargv) {
        for (int i = 1; i < argc; ++i) {
            args.push_back(aoh::wide_to_utf8(wargv[i]));
        }
        LocalFree(wargv);
    }
    int rc = dispatch(args);
    aoh::net_cleanup();
    return rc;
}
#else
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    aoh::net_init();
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(aoh::wide_to_utf8(argv[i]));
    }
    int rc = dispatch(args);
    aoh::net_cleanup();
    return rc;
}
#endif
#else
int main(int argc, char* argv[]) {
    aoh::net_init();
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    int rc = dispatch(args);
    aoh::net_cleanup();
    return rc;
}
#endif
