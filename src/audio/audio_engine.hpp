#pragma once

#include "common/config.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aoh {

struct CachedAudio {
    std::string path;
    bool is_mp3 = true;
    std::vector<uint8_t> raw_bytes;   // Resident in RAM permanently while selected
    std::vector<int16_t> pcm_samples; // Decoded PCM (used on Windows waveOut & built-in ding)
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
};

class PlatformAudioBackend {
public:
    virtual ~PlatformAudioBackend() = default;
    virtual std::string name() const = 0;
    virtual bool init() = 0;
    virtual void shutdown() = 0;

    virtual bool prepare_mp3_in_memory(CachedAudio& audio, std::string& err) = 0;
    virtual void play_cached_audio_blocking(const CachedAudio& audio, int volume_0_100,
                                            std::atomic<bool>& cancel_flag) = 0;
    virtual void stop_audio() = 0;
};

std::unique_ptr<PlatformAudioBackend> create_platform_audio_backend();

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool init();
    void shutdown();

    void sync_config(const AppConfig& cfg);
    bool preload_mp3(const std::string& utf8_path, std::string& err);

    // Non-overlapping: immediately cuts off any currently playing sound
    void play_exit(const AppConfig& cfg);
    void play_idle(const AppConfig& cfg);
    void play_mp3_path(const std::string& utf8_path, int volume_0_100);
    void stop();

    std::string backend_info() const;
    size_t cached_mp3_count() const;
    size_t cached_mp3_bytes() const;
    std::string last_error() const;

private:
    std::shared_ptr<const CachedAudio> make_builtin_ding(bool two_tone_idle) const;
    std::shared_ptr<const CachedAudio> get_or_load_mp3(const std::string& path, std::string& err);
    void play_slot_or_random(const SoundSlotConfig& slot, const AppConfig& cfg, bool is_idle);
    void play_random_from_pool(const AppConfig& cfg, bool is_idle);
    void start_playback(std::shared_ptr<const CachedAudio> audio, int volume_0_100);
    void stop_locked(std::unique_lock<std::mutex>& lk);
    void set_error(const std::string& err);

    std::unique_ptr<PlatformAudioBackend> backend_;
    std::shared_ptr<const CachedAudio> builtin_exit_ding_;
    std::shared_ptr<const CachedAudio> builtin_idle_ding_;

    mutable std::mutex cache_mu_;
    std::unordered_map<std::string, std::shared_ptr<const CachedAudio>> mp3_cache_;

    mutable std::mutex rng_mu_;
    std::mt19937 rng_;
    std::string last_random_path_;

    mutable std::mutex play_mu_;
    std::atomic<bool> cancel_flag_{false};
    std::thread play_thread_;

    mutable std::mutex err_mu_;
    std::string last_error_;
};

} // namespace aoh
