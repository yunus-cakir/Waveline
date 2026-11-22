#include "audio.h"

#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wl {

namespace {

const PROPERTYKEY kPkeySonarVadRole = {
    {0xF141E7CC, 0x921B, 0x466D, {0x80, 0x2F, 0x87, 0x9D, 0x6D, 0x4A, 0xEC, 0x17}}, 0};

// Packets whose timestamp is within 2 ms of where we expect them are treated as
// contiguous; this absorbs timestamp jitter without inserting tiny gaps/overlaps.
constexpr int64_t kJitterFrames = kSampleRate / 500;
// The engine runs this far behind real time so late packets still make it in.
constexpr int64_t kEngineLatencyFrames = kSampleRate * 150 / 1000;
constexpr REFERENCE_TIME kDeviceBufferHns = 1'000'000;  // 100 ms

com_ptr<IMMDeviceEnumerator> CreateEnumerator() {
    com_ptr<IMMDeviceEnumerator> e;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(e.put())),
          "CoCreateInstance(MMDeviceEnumerator)");
    return e;
}

int ReadSonarRole(IPropertyStore* props) {
    PROPVARIANT v;
    PropVariantInit(&v);
    int role = kSonarNone;
    if (SUCCEEDED(props->GetValue(kPkeySonarVadRole, &v))) {
        switch (v.vt) {
            case VT_UI4: role = static_cast<int>(v.ulVal); break;
            case VT_I4: role = v.lVal; break;
            case VT_UI2: role = v.uiVal; break;
            case VT_UI1: role = v.bVal; break;
            default: break;
        }
    }
    PropVariantClear(&v);
    return role;
}

}  // namespace

const char* TrackName(Track t) {
    switch (t) {
        case Track::Mix: return "Mix";
        case Track::Game: return "Game";
        case Track::Chat: return "Chat";
        case Track::Media: return "Media";
        case Track::Mic: return "Mic";
        default: return "?";
    }
}

std::vector<EndpointInfo> EnumerateEndpoints() {
    auto enumerator = CreateEnumerator();
    com_ptr<IMMDeviceCollection> devices;
    Check(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, devices.put()), "EnumAudioEndpoints");
    UINT count = 0;
    devices->GetCount(&count);

    std::vector<EndpointInfo> result;
    for (UINT i = 0; i < count; ++i) {
        com_ptr<IMMDevice> device;
        if (FAILED(devices->Item(i, device.put()))) continue;
        EndpointInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }
        if (auto endpoint = device.try_as<IMMEndpoint>()) {
            EDataFlow flow = eRender;
            endpoint->GetDataFlow(&flow);
            info.capture = flow == eCapture;
        }
        com_ptr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
                info.name = name.pwszVal;
            PropVariantClear(&name);
            info.sonarRole = ReadSonarRole(props.get());
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::wstring DefaultEndpointId(bool capture) {
    auto enumerator = CreateEnumerator();
    com_ptr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, device.put())))
        return {};
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(device->GetId(&id))) {
        out = id;
        CoTaskMemFree(id);
    }
    return out;
}

// ---------------------------------------------------------------- AudioSource

AudioSource::AudioSource(std::wstring deviceId, std::string label, bool loopback)
    : deviceId_(std::move(deviceId)), label_(std::move(label)), loopback_(loopback) {}

AudioSource::~AudioSource() {
    Stop();
    if (event_) CloseHandle(event_);
}

void AudioSource::Open() {
    auto enumerator = CreateEnumerator();
    com_ptr<IMMDevice> device;
    Check(enumerator->GetDevice(deviceId_.c_str(), device.put()), "IMMDeviceEnumerator::GetDevice");
    Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void()),
          "IMMDevice::Activate(IAudioClient)");

    // Ask WASAPI for 48 kHz float stereo directly; it resamples/remixes if the device runs
    // in another format.
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = kChannels;
    wf.Format.nSamplesPerSec = kSampleRate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = kChannels * 4;
    wf.Format.nAvgBytesPerSec = kSampleRate * wf.Format.nBlockAlign;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    Check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kDeviceBufferHns, 0, &wf.Format, nullptr),
          "IAudioClient::Initialize");

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) Check(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent");
    Check(client_->SetEventHandle(event_), "IAudioClient::SetEventHandle");
    Check(client_->GetService(IID_PPV_ARGS(capture_.put())), "IAudioClient::GetService(capture)");
}

void AudioSource::Start(int64_t startHns) {
    startHns_ = startHns;
    running_ = true;
    Check(client_->Start(), "IAudioClient::Start");
    thread_ = std::thread(&AudioSource::Run, this);
}

void AudioSource::Stop() {
    if (!running_.exchange(false)) return;
    if (event_) SetEvent(event_);
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
}

void AudioSource::Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    std::vector<float> temp;
    while (running_) {
        WaitForSingleObject(event_, 100);
        if (!running_) break;

        UINT32 next = 0;
        HRESULT hr = capture_->GetNextPacketSize(&next);
        while (SUCCEEDED(hr) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePos = 0, qpcPos = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, &devicePos, &qpcPos);
            if (FAILED(hr)) break;

            temp.resize(static_cast<size_t>(frames) * kChannels);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data)
                std::fill(temp.begin(), temp.end(), 0.0f);
            else
                std::memcpy(temp.data(), data, temp.size() * sizeof(float));

            {
                std::lock_guard lock(mutex_);
                ++stats_.packets;
                stats_.frames += frames;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) ++stats_.silentPackets;
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++stats_.discontinuities;
            }

            const int64_t pos = (static_cast<int64_t>(qpcPos) - startHns_) * kSampleRate / kHns;
            Push(pos, temp.data(), frames);

            capture_->ReleaseBuffer(frames);
            hr = capture_->GetNextPacketSize(&next);
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            // M0 only reports this; M5 reopens the device (e.g. when Sonar restarts).
            std::lock_guard lock(mutex_);
            if (!stats_.deviceLost) Warn("%s: ses cihazi kayboldu, iz sessizlikle devam ediyor", label_.c_str());
            stats_.deviceLost = true;
            running_ = false;
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void AudioSource::Push(int64_t pos, const float* data, uint32_t frames) {
    if (pos < 0) {  // packet started before the recording
        const int64_t skip = -pos;
        if (skip >= frames) return;
        data += skip * kChannels;
        frames -= static_cast<uint32_t>(skip);
        pos = 0;
    }

    std::lock_guard lock(mutex_);
    int64_t diff = pos - WritePosLocked();
    if (diff >= -kJitterFrames && diff <= kJitterFrames) diff = 0;

    if (diff > 0) {
        fifo_.insert(fifo_.end(), static_cast<size_t>(diff) * kChannels, 0.0f);
        stats_.gapFilledFrames += static_cast<uint64_t>(diff);
    } else if (diff < 0) {
        const int64_t drop = -diff;
        if (drop >= frames) {
            stats_.overlapDroppedFrames += frames;
            return;
        }
        data += drop * kChannels;
        frames -= static_cast<uint32_t>(drop);
        stats_.overlapDroppedFrames += static_cast<uint64_t>(drop);
    }
    fifo_.insert(fifo_.end(), data, data + static_cast<size_t>(frames) * kChannels);
}

void AudioSource::ReadUntil(int64_t untilFrame, std::vector<float>& out) {
    std::lock_guard lock(mutex_);
    const int64_t writePos = WritePosLocked();
    if (writePos < untilFrame) {
        // Loopback delivers no packets while nothing plays: that time is silence.
        fifo_.insert(fifo_.end(), static_cast<size_t>(untilFrame - writePos) * kChannels, 0.0f);
        stats_.deadlineFilledFrames += static_cast<uint64_t>(untilFrame - writePos);
    }
    const int64_t n = std::max<int64_t>(0, untilFrame - fifoPos_);
    const size_t samples = static_cast<size_t>(n) * kChannels;
    out.assign(fifo_.begin(), fifo_.begin() + static_cast<ptrdiff_t>(samples));
    fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<ptrdiff_t>(samples));
    fifoPos_ = untilFrame;
}

SourceStats AudioSource::Stats() {
    std::lock_guard lock(mutex_);
    return stats_;
}

// ---------------------------------------------------------------- AudioEngine

void AudioEngine::SetSource(Track track, std::unique_ptr<AudioSource> source) {
    sources_[static_cast<int>(track)] = std::move(source);
}

void AudioEngine::Start(IAudioSink* writer, int64_t startHns) {
    writer_ = writer;
    startHns_ = startHns;
    mixPos_ = 0;
    for (auto& s : sources_)
        if (s) s->Start(startHns);
    thread_ = std::thread(&AudioEngine::Run, this);
}

AudioEngine::~AudioEngine() {
    // Error path (e.g. an exception in the video loop): stop without flushing more audio.
    if (thread_.joinable()) {
        abort_ = true;
        thread_.join();
    }
    for (auto& s : sources_)
        if (s) s->Stop();
}

void AudioEngine::StopAt(int64_t endFrame) {
    stopFrame_ = endFrame;
    if (thread_.joinable()) thread_.join();
    for (auto& s : sources_)
        if (s) s->Stop();
}

void AudioEngine::Run() {
    try {
        RunLoop();
    } catch (const HrError& e) {
        Warn("Ses motoru durdu: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
        failed_ = true;
    } catch (const std::exception& e) {
        Warn("Ses motoru durdu: %s", e.what());
        failed_ = true;
    }
}

void AudioEngine::RunLoop() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!abort_) {
        Sleep(20);
        const int64_t now = (QpcNow100ns() - startHns_) * kSampleRate / kHns;
        int64_t target = now - kEngineLatencyFrames;
        const int64_t stop = stopFrame_.load();
        if (stop >= 0 && target >= stop) {
            Process(stop);
            break;
        }
        if (target > mixPos_) Process(target);
    }
}

void AudioEngine::Process(int64_t untilFrame) {
    const int64_t n = untilFrame - mixPos_;
    if (n <= 0) return;
    const size_t samples = static_cast<size_t>(n) * kChannels;

    auto& mix = buffers_[static_cast<int>(Track::Mix)];
    mix.assign(samples, 0.0f);
    for (int t = static_cast<int>(Track::Game); t < kTrackCount; ++t) {
        auto& buf = buffers_[t];
        if (sources_[t])
            sources_[t]->ReadUntil(untilFrame, buf);
        else
            buf.assign(samples, 0.0f);
        for (size_t i = 0; i < samples; ++i) mix[i] += buf[i];
    }

    const int64_t timeHns = FramesToHns(mixPos_);
    pcm_.resize(samples);
    for (int t = 0; t < kTrackCount; ++t) {
        const auto& buf = buffers_[t];
        for (size_t i = 0; i < samples; ++i) {
            float v = buf[i];
            if (v > 1.0f || v < -1.0f) {
                if (t == static_cast<int>(Track::Mix)) ++mixClipped_;
                v = std::clamp(v, -1.0f, 1.0f);
            }
            pcm_[i] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
        }
        writer_->WriteAudio(static_cast<UINT>(t), pcm_.data(), static_cast<uint32_t>(n), timeHns);
    }
    mixPos_ = untilFrame;
}

}  // namespace wl
