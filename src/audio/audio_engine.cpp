#include "audio/audio_engine.hpp"
#include "audio/embedded_sounds.hpp"
#include "common/utf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_set>

namespace aoh {

AudioEngine::AudioEngine()
    : backend_(create_platform_audio_backend()), rng_(std::random_device{}()) {}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
    if (!backend_ || !backend_->init()) return false;
    builtin_exit_ding_ = make_builtin_ding(false);
    builtin_idle_ding_ = make_builtin_ding(true);
    return true;
}

void AudioEngine::shutdown() {
    {
        std::unique_lock<std::mutex> lk(play_mu_);
        stop_locked(lk);
    }
    if (backend_) backend_->shutdown();
    std::lock_guard<std::mutex> clk(cache_mu_);
    mp3_cache_.clear();
}

void AudioEngine::set_error(const std::string& err) {
    std::lock_guard<std::mutex> lk(err_mu_);
    last_error_ = err;
}

std::string AudioEngine::last_error() const {
    std::lock_guard<std::mutex> lk(err_mu_);
    return last_error_;
}

std::string AudioEngine::backend_info() const {
    return backend_ ? backend_->name() : "none";
}

size_t AudioEngine::cached_mp3_count() const {
    std::lock_guard<std::mutex> lk(cache_mu_);
    return mp3_cache_.size();
}

size_t AudioEngine::cached_mp3_bytes() const {
    std::lock_guard<std::mutex> lk(cache_mu_);
    size_t total = 0;
    for (const auto& kv : mp3_cache_) {
        total += kv.second->raw_bytes.size();
        total += kv.second->pcm_samples.size() * sizeof(int16_t);
    }
    return total;
}

std::shared_ptr<const CachedAudio> AudioEngine::make_builtin_ding(bool two_tone_idle) const {
    constexpr uint32_t sr = 44100;
    constexpr double pi2 = 6.283185307179586;
    const size_t total_samples = sr * 55 / 100; // 0.55s

    auto item = std::make_shared<CachedAudio>();
    item->path = two_tone_idle ? "<builtin:idle_ding>" : "<builtin:exit_ding>";
    item->is_mp3 = false;
    item->sample_rate = sr;
    item->channels = 1;
    item->pcm_samples.resize(total_samples, 0);

    double f1 = two_tone_idle ? 659.25 : 783.99;  // E5 or G5
    double f2 = two_tone_idle ? 880.00 : 1046.50; // A5 or C6
    size_t second_start = sr * 15 / 100;          // 150ms

    for (size_t i = 0; i < total_samples; ++i) {
        double t1 = static_cast<double>(i) / sr;
        double env1 = std::exp(-t1 * 12.0) * std::min(1.0, t1 / 0.004);
        double sig = (std::sin(pi2 * f1 * t1) + 0.25 * std::sin(pi2 * f1 * 2.0 * t1)) * env1;

        if (i >= second_start) {
            double t2 = static_cast<double>(i - second_start) / sr;
            double env2 = std::exp(-t2 * 9.5) * std::min(1.0, t2 / 0.004);
            sig += (std::sin(pi2 * f2 * t2) + 0.2 * std::sin(pi2 * f2 * 2.0 * t2)) * env2;
        }
        double clamped = std::clamp(sig * 0.45, -1.0, 1.0);
        item->pcm_samples[i] = static_cast<int16_t>(clamped * 32767.0);
    }

    uint32_t data_bytes = static_cast<uint32_t>(item->pcm_samples.size() * sizeof(int16_t));
    uint32_t riff_bytes = 36 + data_bytes;
    uint8_t hdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
        16,0,0,0, 1,0, 1,0, 0x44,0xAC,0,0, 0x88,0x58,0x01,0, 2,0, 16,0,
        'd','a','t','a', 0,0,0,0
    };
    std::memcpy(hdr + 4, &riff_bytes, 4);
    std::memcpy(hdr + 40, &data_bytes, 4);
    item->raw_bytes.resize(44 + data_bytes);
    std::memcpy(item->raw_bytes.data(), hdr, 44);
    std::memcpy(item->raw_bytes.data() + 44, item->pcm_samples.data(), data_bytes);
    return item;
}

std::shared_ptr<const CachedAudio> AudioEngine::get_or_load_mp3(const std::string& path, std::string& err) {
    if (path.empty()) {
        err = "MP3 path is empty";
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        auto it = mp3_cache_.find(path);
        if (it != mp3_cache_.end()) {
            return it->second; // Resident in RAM, zero disk read
        }
    }

    auto item = std::make_shared<CachedAudio>();
    item->path = path;
    item->is_mp3 = true;

    if (const auto* emb = find_embedded_sound(path)) {
        item->raw_bytes.assign(emb->data, emb->data + emb->size);
    } else {
        std::ifstream ifs(path_from_utf8(path), std::ios::binary);
        if (!ifs.is_open()) {
            err = "Cannot open audio file: " + path;
            return nullptr;
        }
        item->raw_bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }

    if (item->raw_bytes.empty()) {
        err = "Audio file is empty: " + path;
        return nullptr;
    }

    if (!backend_->prepare_mp3_in_memory(*item, err)) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        mp3_cache_[path] = item; // Kept resident in RAM until changed/removed
    }
    return item;
}

bool AudioEngine::preload_mp3(const std::string& utf8_path, std::string& err) {
    auto item = get_or_load_mp3(utf8_path, err);
    if (!item) {
        set_error(err);
        return false;
    }
    set_error("");
    return true;
}

void AudioEngine::sync_config(const AppConfig& cfg) {
    std::unordered_set<std::string> keep;
    auto preload_path = [&](const std::string& p) {
        if (p.empty()) return;
        keep.insert(p);
        std::string err;
        if (!preload_mp3(p, err)) {
            set_error(err);
        }
    };

    if (cfg.exit_sound.mode == "mp3") preload_path(cfg.exit_sound.mp3_path);
    if (cfg.idle_sound.mode == "mp3") preload_path(cfg.idle_sound.mp3_path);
    for (const auto& p : cfg.random_slots) {
        preload_path(trim_copy(p));
    }

    std::lock_guard<std::mutex> lk(cache_mu_);
    for (auto it = mp3_cache_.begin(); it != mp3_cache_.end();) {
        if (keep.find(it->first) == keep.end()) {
            it = mp3_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioEngine::stop_locked(std::unique_lock<std::mutex>& lk) {
    cancel_flag_.store(true, std::memory_order_release);
    if (backend_) backend_->stop_audio();
    if (play_thread_.joinable()) {
        std::thread t = std::move(play_thread_);
        lk.unlock();
        t.join();
        lk.lock();
    }
}

void AudioEngine::stop() {
    std::unique_lock<std::mutex> lk(play_mu_);
    stop_locked(lk);
}

void AudioEngine::start_playback(std::shared_ptr<const CachedAudio> audio, int volume_0_100) {
    if (!backend_ || !audio) return;
    int vol = std::clamp(volume_0_100, 0, 100);

    std::unique_lock<std::mutex> lk(play_mu_);
    stop_locked(lk); // Immediately interrupts previous playback -> no overlap!
    cancel_flag_.store(false, std::memory_order_release);
    play_thread_ = std::thread([this, audio = std::move(audio), vol]() {
        backend_->play_cached_audio_blocking(*audio, vol, cancel_flag_);
    });
}

void AudioEngine::play_random_from_pool(const AppConfig& cfg, bool is_idle) {
    std::vector<std::string> candidates;
    for (const auto& raw : cfg.random_slots) {
        std::string p = trim_copy(raw);
        if (!p.empty()) candidates.push_back(std::move(p));
    }

    if (candidates.empty()) {
        start_playback(is_idle ? builtin_idle_ding_ : builtin_exit_ding_, cfg.volume);
        return;
    }

    std::string chosen;
    {
        std::lock_guard<std::mutex> lk(rng_mu_);
        if (candidates.size() == 1) {
            chosen = candidates[0];
        } else {
            // Avoid repeating the exact same slot twice in a row when >= 2 slots are filled
            std::vector<std::string> pool;
            pool.reserve(candidates.size());
            for (const auto& c : candidates) {
                if (c != last_random_path_) pool.push_back(c);
            }
            const auto& pick_from = pool.empty() ? candidates : pool;
            std::uniform_int_distribution<size_t> dist(0, pick_from.size() - 1);
            chosen = pick_from[dist(rng_)];
        }
        last_random_path_ = chosen;
    }

    std::string err;
    auto cached = get_or_load_mp3(chosen, err);
    if (cached) {
        start_playback(std::move(cached), cfg.volume);
        return;
    }
    set_error(err);
    start_playback(is_idle ? builtin_idle_ding_ : builtin_exit_ding_, cfg.volume);
}

void AudioEngine::play_slot_or_random(const SoundSlotConfig& slot, const AppConfig& cfg, bool is_idle) {
    if (slot.mode == "random") {
        play_random_from_pool(cfg, is_idle);
        return;
    }
    if (slot.mode == "mp3" && !slot.mp3_path.empty()) {
        std::string err;
        auto cached = get_or_load_mp3(slot.mp3_path, err);
        if (cached) {
            start_playback(std::move(cached), cfg.volume);
            return;
        }
        set_error(err);
    }
    start_playback(is_idle ? builtin_idle_ding_ : builtin_exit_ding_, cfg.volume);
}

void AudioEngine::play_exit(const AppConfig& cfg) {
    play_slot_or_random(cfg.exit_sound, cfg, false);
}

void AudioEngine::play_idle(const AppConfig& cfg) {
    play_slot_or_random(cfg.idle_sound, cfg, true);
}

void AudioEngine::play_mp3_path(const std::string& utf8_path, int volume_0_100) {
    if (utf8_path.empty()) {
        start_playback(builtin_exit_ding_, volume_0_100);
        return;
    }
    std::string err;
    auto cached = get_or_load_mp3(utf8_path, err);
    if (cached) {
        start_playback(std::move(cached), volume_0_100);
        return;
    }
    set_error(err);
}

} // namespace aoh
