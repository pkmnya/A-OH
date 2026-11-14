#include "audio/audio_engine.hpp"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <mutex>
#include <thread>

namespace aoh {

namespace {

class MacOsAudioBackend final : public PlatformAudioBackend {
public:
    std::string name() const override {
        return "macOS AppKit NSSound (In-Memory MP3)";
    }

    bool init() override { return true; }
    void shutdown() override { stop_audio(); }

    bool prepare_mp3_in_memory(CachedAudio& audio, std::string& err) override {
        @autoreleasepool {
            NSData* data = [NSData dataWithBytesNoCopy:const_cast<uint8_t*>(audio.raw_bytes.data())
                                                length:audio.raw_bytes.size()
                                          freeWhenDone:NO];
            NSSound* probe = [[NSSound alloc] initWithData:data];
            if (!probe) {
                err = "macOS NSSound failed to parse audio buffer";
                return false;
            }
            return true;
        }
    }

    void play_cached_audio_blocking(const CachedAudio& audio, int volume_0_100,
                                    std::atomic<bool>& cancel_flag) override {
        @autoreleasepool {
            NSData* data = [NSData dataWithBytesNoCopy:const_cast<uint8_t*>(audio.raw_bytes.data())
                                                length:audio.raw_bytes.size()
                                          freeWhenDone:NO];
            NSSound* sound = [[NSSound alloc] initWithData:data];
            if (!sound) return;

            [sound setVolume:static_cast<float>(std::clamp(volume_0_100, 0, 100)) / 100.0f];
            {
                std::lock_guard<std::mutex> lk(audio_mu_);
                active_sound_ = sound;
            }
            [sound play];
            while (!cancel_flag.load(std::memory_order_acquire) && [sound isPlaying]) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            if (cancel_flag.load(std::memory_order_acquire)) {
                [sound stop];
            }
            {
                std::lock_guard<std::mutex> lk(audio_mu_);
                if (active_sound_ == sound) active_sound_ = nil;
            }
        }
    }

    void stop_audio() override {
        std::lock_guard<std::mutex> lk(audio_mu_);
        if (active_sound_) {
            [active_sound_ stop];
            active_sound_ = nil;
        }
    }

private:
    mutable std::mutex audio_mu_;
    NSSound* active_sound_ = nil;
};

} // namespace

std::unique_ptr<PlatformAudioBackend> create_platform_audio_backend() {
    return std::make_unique<MacOsAudioBackend>();
}

} // namespace aoh
