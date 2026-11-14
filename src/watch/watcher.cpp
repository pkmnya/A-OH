#include "watch/watcher.hpp"
#include "audio/audio_engine.hpp"
#include "common/config.hpp"
#include "common/utf8.hpp"
#include "daemon/daemon.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

namespace aoh {

namespace {

void trigger_sound_or_local(const AppConfig& cfg, bool is_idle) {
    std::string reply;
    if (send_ipc_command(cfg.ipc_port, is_idle ? "PLAY_IDLE" : "PLAY_EXIT", reply)) {
        return;
    }
    // Fallback: play directly if daemon isn't running
    AudioEngine eng;
    if (eng.init()) {
        if (is_idle) eng.play_idle(cfg);
        else eng.play_exit(cfg);
        // Wait briefly for standalone playback when exiting
        if (!is_idle) std::this_thread::sleep_for(std::chrono::seconds(2));
        eng.shutdown();
    }
}

} // namespace

int run_watch_command(int idle_seconds, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: a-oh watch [--idle N] -- <command...>\n";
        return 2;
    }

    AppConfig cfg = load_config();
    int threshold_s = idle_seconds > 0 ? idle_seconds : cfg.idle_seconds;

    std::atomic<bool> finished{false};
    std::mutex activity_mu;
    auto last_activity = std::chrono::steady_clock::now();
    bool idle_notified = false;

    std::thread idle_monitor([&]() {
        while (!finished.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (finished.load(std::memory_order_acquire)) break;
            bool should_notify = false;
            {
                std::lock_guard<std::mutex> lk(activity_mu);
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - last_activity).count();
                if (!idle_notified && elapsed >= threshold_s) {
                    idle_notified = true;
                    should_notify = true;
                }
            }
            if (should_notify) {
                trigger_sound_or_local(cfg, true);
            }
        }
    });

    int exit_code = 0;

#ifdef _WIN32
    std::wstring cmdline = L"cmd.exe /c";
    for (const auto& a : args) {
        cmdline += L" ";
        bool need_quote = a.find(' ') != std::string::npos || a.find('\t') != std::string::npos;
        std::wstring wa = utf8_to_wide(a);
        if (need_quote) cmdline += L"\"" + wa + L"\"";
        else cmdline += wa;
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        finished.store(true, std::memory_order_release);
        idle_monitor.join();
        return 1;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        finished.store(true, std::memory_order_release);
        idle_monitor.join();
        std::cerr << "Failed to launch command\n";
        return 1;
    }
    CloseHandle(write_pipe);

    char buf[4096];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        std::cout.write(buf, bytes_read);
        std::cout.flush();
        std::lock_guard<std::mutex> lk(activity_mu);
        last_activity = std::chrono::steady_clock::now();
        idle_notified = false;
    }
    CloseHandle(read_pipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    exit_code = static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        finished.store(true, std::memory_order_release);
        idle_monitor.join();
        return 1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        std::cout.write(buf, n);
        std::cout.flush();
        std::lock_guard<std::mutex> lk(activity_mu);
        last_activity = std::chrono::steady_clock::now();
        idle_notified = false;
    }
    close(pipefd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
#endif

    finished.store(true, std::memory_order_release);
    idle_monitor.join();

    trigger_sound_or_local(cfg, false);
    return exit_code;
}

void print_shell_hook(const std::string& shell_name) {
    if (shell_name == "pwsh" || shell_name == "powershell") {
        std::cout << "# Add to your PowerShell $PROFILE:\n"
                  << "Register-EngineEvent PowerShell.Exiting -Action { a-oh play exit | Out-Null } | Out-Null\n"
                  << "$global:__AohLastPrompt = Get-Date\n"
                  << "function global:prompt {\n"
                  << "    $now = Get-Date\n"
                  << "    if ((($now - $global:__AohLastPrompt).TotalSeconds) -ge 5) { a-oh play idle | Out-Null }\n"
                  << "    $global:__AohLastPrompt = $now\n"
                  << "    \"PS $($executionContext.SessionState.Path.CurrentLocation)$('>' * ($nestedPromptLevel + 1)) \"\n"
                  << "}\n";
    } else if (shell_name == "zsh") {
        std::cout << "# Add to ~/.zshrc:\n"
                  << "preexec() { __aoh_t0=$SECONDS; }\n"
                  << "precmd() {\n"
                  << "  if [[ -n \"$__aoh_t0\" ]] && (( SECONDS - __aoh_t0 >= 5 )); then\n"
                  << "    (a-oh play idle >/dev/null 2>&1 &)\n"
                  << "  fi\n"
                  << "  unset __aoh_t0\n"
                  << "}\n"
                  << "zshexit() { (a-oh play exit >/dev/null 2>&1 &); }\n";
    } else {
        std::cout << "# Add to ~/.bashrc:\n"
                  << "__aoh_precmd() {\n"
                  << "  if [[ -n \"$__aoh_busy\" ]] && (( SECONDS - __aoh_t0 >= 5 )); then\n"
                  << "    (a-oh play idle >/dev/null 2>&1 &)\n"
                  << "  fi\n"
                  << "  __aoh_busy=\"\"\n"
                  << "}\n"
                  << "trap '__aoh_t0=$SECONDS; __aoh_busy=1' DEBUG\n"
                  << "PROMPT_COMMAND=\"__aoh_precmd${PROMPT_COMMAND:+;$PROMPT_COMMAND}\"\n"
                  << "trap '(a-oh play exit >/dev/null 2>&1 &)' EXIT\n";
    }
}

} // namespace aoh
