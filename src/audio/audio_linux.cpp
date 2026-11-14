#include "audio/audio_engine.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace aoh {

namespace {

bool cmd_exists(const char* cmd) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string full = p.substr(start, end - start) + "/" + cmd;
        if (access(full.c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}

class LinuxAudioBackend final : public PlatformAudioBackend {
public:
    std::string name() const override {
        if (cmd_exists("mpg123")) return "Linux mpg123 (Pipe RAM MP3)";
        if (cmd_exists("ffplay")) return "Linux ffplay (Pipe RAM MP3)";
        return "Linux pw-play/paplay/aplay (Built-in WAV; install mpg123/ffmpeg for MP3)";
    }

    bool init() override { return true; }
    void shutdown() override { stop_audio(); }

    bool prepare_mp3_in_memory(CachedAudio& audio, std::string& err) override {
        if (audio.raw_bytes.empty()) {
            err = "Empty audio buffer";
            return false;
        }
        if (!cmd_exists("mpg123") && !cmd_exists("ffplay")) {
            err = "Please install mpg123 or ffmpeg (ffplay) on Linux for MP3 playback";
            return false;
        }
        return true;
    }

    void play_cached_audio_blocking(const CachedAudio& audio, int volume_0_100,
                                    std::atomic<bool>& cancel_flag) override {
        if (audio.raw_bytes.empty()) return;

        int pipefd[2];
        if (pipe(pipefd) != 0) return;

        pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[1]);
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            if (audio.is_mp3) {
                std::string vol_ff = std::to_string(std::clamp(volume_0_100, 0, 100));
                std::string vol_mpg = std::to_string((std::clamp(volume_0_100, 0, 100) * 32768) / 100);
                if (cmd_exists("mpg123")) {
                    execlp("mpg123", "mpg123", "-q", "-f", vol_mpg.c_str(), "-", nullptr);
                } else if (cmd_exists("ffplay")) {
                    execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
                           "-volume", vol_ff.c_str(), "-i", "pipe:0", nullptr);
                }
            } else {
                if (cmd_exists("pw-play")) {
                    execlp("pw-play", "pw-play", "-", nullptr);
                } else if (cmd_exists("paplay")) {
                    execlp("paplay", "paplay", "-", nullptr);
                } else if (cmd_exists("aplay")) {
                    execlp("aplay", "aplay", "-q", "-", nullptr);
                }
            }
            _exit(127);
        }

        close(pipefd[0]);
        if (pid < 0) {
            close(pipefd[1]);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(pid_mu_);
            active_pid_ = pid;
        }

        size_t offset = 0;
        while (offset < audio.raw_bytes.size() && !cancel_flag.load(std::memory_order_acquire)) {
            ssize_t w = write(pipefd[1], audio.raw_bytes.data() + offset, audio.raw_bytes.size() - offset);
            if (w <= 0) break;
            offset += static_cast<size_t>(w);
        }
        close(pipefd[1]);

        int status = 0;
        while (!cancel_flag.load(std::memory_order_acquire)) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || r < 0) break;
            usleep(15000);
        }
        if (cancel_flag.load(std::memory_order_acquire)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }

        {
            std::lock_guard<std::mutex> lk(pid_mu_);
            if (active_pid_ == pid) active_pid_ = -1;
        }
    }

    void stop_audio() override {
        std::lock_guard<std::mutex> lk(pid_mu_);
        if (active_pid_ > 0) {
            kill(active_pid_, SIGKILL);
            int st = 0;
            waitpid(active_pid_, &st, 0);
            active_pid_ = -1;
        }
    }

private:
    mutable std::mutex pid_mu_;
    pid_t active_pid_ = -1;
};

} // namespace

std::unique_ptr<PlatformAudioBackend> create_platform_audio_backend() {
    return std::make_unique<LinuxAudioBackend>();
}

} // namespace aoh
