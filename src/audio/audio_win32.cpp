#include "audio/audio_engine.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace aoh {

namespace {

using PFN_MFCreateMFByteStreamOnStream = HRESULT(WINAPI*)(IStream*, IMFByteStream**);

class Win32AudioBackend final : public PlatformAudioBackend {
public:
    std::string name() const override {
        return "Windows Media Foundation + WinMM (In-Memory MP3)";
    }

    bool init() override {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        mf_started_ = SUCCEEDED(hr);
        return true;
    }

    void shutdown() override {
        stop_audio();
        if (mf_started_) {
            MFShutdown();
            mf_started_ = false;
        }
    }

    bool prepare_mp3_in_memory(CachedAudio& audio, std::string& err) override {
        HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool need_co_uninit = (co_hr == S_OK || co_hr == S_FALSE);
        auto cleanup_com = [&]() {
            if (need_co_uninit) CoUninitialize();
        };

        HMODULE mfplat = GetModuleHandleW(L"mfplat.dll");
        if (!mfplat) mfplat = LoadLibraryW(L"mfplat.dll");
        auto fnCreateByteStream = mfplat
            ? reinterpret_cast<PFN_MFCreateMFByteStreamOnStream>(GetProcAddress(mfplat, "MFCreateMFByteStreamOnStream"))
            : nullptr;
        if (!fnCreateByteStream) {
            err = "MFCreateMFByteStreamOnStream unavailable";
            cleanup_com();
            return false;
        }

        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, audio.raw_bytes.size());
        if (!hMem) {
            err = "GlobalAlloc failed";
            cleanup_com();
            return false;
        }
        void* locked = GlobalLock(hMem);
        if (!locked) {
            GlobalFree(hMem);
            err = "GlobalLock failed";
            cleanup_com();
            return false;
        }
        std::memcpy(locked, audio.raw_bytes.data(), audio.raw_bytes.size());
        GlobalUnlock(hMem);

        IStream* pStream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &pStream)) || !pStream) {
            GlobalFree(hMem);
            err = "CreateStreamOnHGlobal failed";
            cleanup_com();
            return false;
        }

        IMFByteStream* pByteStream = nullptr;
        if (FAILED(fnCreateByteStream(pStream, &pByteStream)) || !pByteStream) {
            pStream->Release();
            err = "MFCreateMFByteStreamOnStream failed";
            cleanup_com();
            return false;
        }

        IMFSourceReader* pReader = nullptr;
        if (FAILED(MFCreateSourceReaderFromByteStream(pByteStream, nullptr, &pReader)) || !pReader) {
            pByteStream->Release();
            pStream->Release();
            err = "Failed to decode MP3 via Media Foundation";
            cleanup_com();
            return false;
        }

        IMFMediaType* pOutType = nullptr;
        HRESULT hr = MFCreateMediaType(&pOutType);
        if (SUCCEEDED(hr)) {
            pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            pOutType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            pOutType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            hr = pReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pOutType);
            pOutType->Release();
        }
        if (FAILED(hr)) {
            pReader->Release();
            pByteStream->Release();
            pStream->Release();
            err = "Media Foundation PCM configuration failed";
            cleanup_com();
            return false;
        }

        IMFMediaType* pActualType = nullptr;
        UINT32 sample_rate = 44100;
        UINT32 channels = 2;
        if (SUCCEEDED(pReader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &pActualType)) && pActualType) {
            pActualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate);
            pActualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            pActualType->Release();
        }
        audio.sample_rate = sample_rate;
        audio.channels = static_cast<uint16_t>(std::max<UINT32>(1, channels));
        audio.pcm_samples.clear();

        while (true) {
            DWORD stream_idx = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* pSample = nullptr;
            hr = pReader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                     0, &stream_idx, &flags, &ts, &pSample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                if (pSample) pSample->Release();
                break;
            }
            if (!pSample) continue;

            IMFMediaBuffer* pBuf = nullptr;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuf)) && pBuf) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(pBuf->Lock(&data, nullptr, &len)) && data && len >= 2) {
                    size_t n = len / sizeof(int16_t);
                    size_t old_sz = audio.pcm_samples.size();
                    audio.pcm_samples.resize(old_sz + n);
                    std::memcpy(audio.pcm_samples.data() + old_sz, data, n * sizeof(int16_t));
                    pBuf->Unlock();
                }
                pBuf->Release();
            }
            pSample->Release();
        }

        pReader->Release();
        pByteStream->Release();
        pStream->Release();
        cleanup_com();

        if (audio.pcm_samples.empty()) {
            err = "Decoded MP3 produced 0 samples";
            return false;
        }
        return true;
    }

    void play_cached_audio_blocking(const CachedAudio& audio, int volume_0_100,
                                    std::atomic<bool>& cancel_flag) override {
        if (audio.pcm_samples.empty() || audio.sample_rate == 0 || audio.channels == 0) return;

        WAVEFORMATEX wfx = {};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = audio.channels;
        wfx.nSamplesPerSec = audio.sample_rate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * 2);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        HANDLE done_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HWAVEOUT hwo = nullptr;
        if (waveOutOpen(&hwo, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(done_ev), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            if (done_ev) CloseHandle(done_ev);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(wave_mu_);
            active_wave_out_ = hwo;
            active_event_ = done_ev;
        }

        std::vector<int16_t> scaled(audio.pcm_samples.size());
        double gain = std::clamp(volume_0_100, 0, 100) / 100.0;
        for (size_t i = 0; i < audio.pcm_samples.size(); ++i) {
            scaled[i] = static_cast<int16_t>(audio.pcm_samples[i] * gain);
        }

        WAVEHDR hdr = {};
        hdr.lpData = reinterpret_cast<LPSTR>(scaled.data());
        hdr.dwBufferLength = static_cast<DWORD>(scaled.size() * sizeof(int16_t));

        ResetEvent(done_ev);
        if (waveOutPrepareHeader(hwo, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) {
            if (waveOutWrite(hwo, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) {
                while (!cancel_flag.load(std::memory_order_acquire)) {
                    if (hdr.dwFlags & WHDR_DONE) break;
                    DWORD r = WaitForSingleObject(done_ev, 25);
                    if (r == WAIT_OBJECT_0 && (hdr.dwFlags & WHDR_DONE)) break;
                }
            }
            if (cancel_flag.load(std::memory_order_acquire)) {
                waveOutReset(hwo);
            }
            waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
        }

        {
            std::lock_guard<std::mutex> lk(wave_mu_);
            if (active_wave_out_ == hwo) active_wave_out_ = nullptr;
            if (active_event_ == done_ev) active_event_ = nullptr;
        }
        waveOutClose(hwo);
        if (done_ev) CloseHandle(done_ev);
    }

    void stop_audio() override {
        std::lock_guard<std::mutex> lk(wave_mu_);
        if (active_wave_out_) waveOutReset(active_wave_out_);
        if (active_event_) SetEvent(active_event_);
    }

private:
    bool mf_started_ = false;
    mutable std::mutex wave_mu_;
    HWAVEOUT active_wave_out_ = nullptr;
    HANDLE active_event_ = nullptr;
};

} // namespace

std::unique_ptr<PlatformAudioBackend> create_platform_audio_backend() {
    return std::make_unique<Win32AudioBackend>();
}

} // namespace aoh
