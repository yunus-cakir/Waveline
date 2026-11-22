#include "session.h"
#include "clip_saver.h"

#include <mferror.h>
#include <mmsystem.h>

#include <filesystem>

#pragma comment(lib, "winmm.lib")

namespace wl {

namespace {

com_ptr<IMFMediaType> MakeNv12Type(UINT w, UINT h, UINT fps) {
    com_ptr<IMFMediaType> t;
    Check(MFCreateMediaType(t.put()), "MFCreateMediaType");
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(t.get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t.get(), MF_MT_FRAME_RATE, fps, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return t;
}

std::wstring SafeFileName(std::wstring s) {
    for (auto& c : s)
        if (wcschr(L"<>:\"/\\|?* ", c) || c < 32) c = L'-';
    return s.empty() ? L"Clip" : s;
}

std::wstring ClipPath(const std::wstring& dir, const std::wstring& target) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf_s(stamp, L"__%04u-%02u-%02u__%02u-%02u-%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring base = dir + L"\\" + SafeFileName(target) + stamp;
    std::wstring path = base + L".mp4";
    for (int i = 2; std::filesystem::exists(path); ++i) path = base + L"_" + std::to_wstring(i) + L".mp4";
    return path;
}

}  // namespace

void SetupAudioSources(AudioEngine& engine, bool logChoices) {
    const auto endpoints = EnumerateEndpoints();
    auto find = [&](int role, bool capture, const wchar_t* hint) -> const EndpointInfo* {
        for (const auto& e : endpoints)
            if (e.capture == capture && e.sonarRole == role) return &e;
        for (const auto& e : endpoints)
            if (e.capture == capture && e.name.find(hint) != std::wstring::npos) return &e;
        return nullptr;
    };
    struct Want { Track track; int role; bool capture; const wchar_t* hint; };
    const Want wants[] = {
        {Track::Game, kSonarGame, false, L"Sonar - Gaming"},
        {Track::Chat, kSonarChat, false, L"Sonar - Chat"},
        {Track::Media, kSonarMedia, false, L"Sonar - Media"},
        {Track::Mic, kSonarMic, true, L"Sonar - Microphone"},
    };
    const bool sonar = find(kSonarGame, false, L"Sonar - Gaming") != nullptr;
    if (!sonar && logChoices) Warn("Sonar bulunamadi: Game = varsayilan hoparlor, Mic = varsayilan mikrofon, Chat/Media sessiz");

    for (const auto& w : wants) {
        std::wstring id, name;
        if (const EndpointInfo* e = find(w.role, w.capture, w.hint)) {
            id = e->id;
            name = e->name;
        } else if (!sonar && (w.track == Track::Game || w.track == Track::Mic)) {
            id = DefaultEndpointId(w.capture);
            for (const auto& c : endpoints)
                if (c.id == id) name = c.name;
        }
        if (id.empty()) {
            if (logChoices) Warn("%s izi icin cihaz yok (sessiz iz)", TrackName(w.track));
            continue;
        }
        auto src = std::make_unique<AudioSource>(id, TrackName(w.track), !w.capture);
        // After Sonar restarts (or the device is re-plugged) find the endpoint again by role.
        src->SetResolver([role = w.role, capture = w.capture, hint = std::wstring(w.hint), track = w.track, sonar]() {
            const auto now = EnumerateEndpoints();
            for (const auto& e : now)
                if (e.capture == capture && e.sonarRole == role) return e.id;
            for (const auto& e : now)
                if (e.capture == capture && e.name.find(hint) != std::wstring::npos) return e.id;
            if (!sonar && (track == Track::Game || track == Track::Mic)) return DefaultEndpointId(capture);
            return std::wstring();
        });
        try {
            src->Open();
            if (logChoices) Log("Ses: %-5s <- %s", TrackName(w.track), ToUtf8(name).c_str());
            engine.SetSource(w.track, std::move(src));
        } catch (const HrError& e) {
            Warn("%s izi acilamadi (%s, 0x%08X)", TrackName(w.track), e.what(), static_cast<unsigned>(e.hr));
        }
    }
}

RecordingSession::RecordingSession(SessionConfig config) : config_(std::move(config)) {}

RecordingSession::~RecordingSession() {
    try {
        Stop();
    } catch (...) {
    }
}

void RecordingSession::Start() {
    gpu_ = CreateGpuDevice();
    UINT resetToken = 0;
    Check(MFCreateDXGIDeviceManager(&resetToken, deviceManager_.put()), "MFCreateDXGIDeviceManager");
    Check(deviceManager_->ResetDevice(gpu_.device.get(), resetToken), "ResetDevice");

    capture_ = std::make_unique<CaptureSource>(config_.item);
    capture_->Start(gpu_, config_.cursor);
    CapturedFrame first;
    const int64_t waitUntil = QpcNow100ns() + 3 * kHns;
    while (!capture_->TakeLatest(first)) {
        if (QpcNow100ns() > waitUntil) throw std::runtime_error("3 saniyede hic kare gelmedi");
        Sleep(5);
    }
    outW_ = first.contentWidth & ~1u;
    outH_ = first.contentHeight & ~1u;
    converter_.Init(gpu_, outW_, outH_, config_.fps);
    converter_.Upload(first.texture.get(), first.contentWidth, first.contentHeight);
    first.frame.Close();
    first = {};

    Check(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(allocator_.put())), "MFCreateVideoSampleAllocatorEx");
    Check(allocator_->SetDirectXManager(deviceManager_.get()), "SetDirectXManager");
    com_ptr<IMFAttributes> attrs;
    Check(MFCreateAttributes(attrs.put(), 2), "MFCreateAttributes");
    attrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER);
    attrs->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
    Check(allocator_->InitializeSampleAllocatorEx(4, 16, attrs.get(), MakeNv12Type(outW_, outH_, config_.fps).get()),
          "InitializeSampleAllocatorEx");

    ring_.SetDuration(static_cast<int64_t>(config_.minutes * 60.0 * kHns));
    video_.Init({outW_, outH_, config_.fps, config_.kbps, config_.hevc}, deviceManager_.get(),
                [this](Packet&& p) { OnVideoPacket(std::move(p)); });
    aac_.Init(kTrackCount, 192, [this](UINT track, Packet&& p) { OnAudioPacket(track, std::move(p)); });
    SetupAudioSources(audio_, false);

    thread_ = std::thread(&RecordingSession::Loop, this);
    Log("Kayit basladi: %s (%ux%u @ %u, %s)", ToUtf8(config_.name).c_str(), outW_, outH_, config_.fps,
        config_.hevc ? "HEVC" : "H.264");
}

void RecordingSession::Loop() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const UINT fps = config_.fps;
    const int64_t frameHns = kHns / fps;
    const int64_t startHns = QpcNow100ns();

    try {
        audio_.Start(&aac_, startHns);
        for (; !stop_ && !capture_->Closed(); ++frames_) {
            const int64_t due = startHns + static_cast<int64_t>(frames_) * kHns / fps;
            const int64_t now = QpcNow100ns();
            if (due > now) {
                LARGE_INTEGER li;
                li.QuadPart = -(due - now);
                SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            }

            CapturedFrame f;
            if (capture_->TakeLatest(f)) {
                converter_.Upload(f.texture.get(), f.contentWidth, f.contentHeight);
                f.frame.Close();
            }

            com_ptr<IMFSample> sample;
            if (SUCCEEDED(allocator_->AllocateSample(sample.put()))) {
                com_ptr<IMFMediaBuffer> buffer;
                Check(sample->GetBufferByIndex(0, buffer.put()), "GetBufferByIndex");
                auto dxgi = buffer.as<IMFDXGIBuffer>();
                com_ptr<ID3D11Texture2D> tex;
                Check(dxgi->GetResource(IID_PPV_ARGS(tex.put())), "GetResource");
                UINT sub = 0;
                dxgi->GetSubresourceIndex(&sub);
                if (auto b2 = buffer.try_as<IMF2DBuffer>()) {
                    DWORD len = 0;
                    if (SUCCEEDED(b2->GetContiguousLength(&len))) buffer->SetCurrentLength(len);
                }
                converter_.Convert(tex.get(), sub);
                video_.Encode(sample.get(), static_cast<int64_t>(frames_) * kHns / fps, frameHns);
            }

            if (saveRequested_.exchange(false)) SaveNow();
            if (frames_ % fps == 0) CheckRecorderHealth();
        }
    } catch (const HrError& e) {
        Warn("Kayit durdu (%s): %s (0x%08X)", ToUtf8(config_.name).c_str(), e.what(), static_cast<unsigned>(e.hr));
    } catch (const std::exception& e) {
        Warn("Kayit durdu (%s): %s", ToUtf8(config_.name).c_str(), e.what());
    } catch (const winrt::hresult_error& e) {
        Warn("Kayit durdu (%s): %s", ToUtf8(config_.name).c_str(), ToUtf8(e.message().c_str()).c_str());
    }
    if (timer) CloseHandle(timer);
    ended_ = true;
}

void RecordingSession::SaveNow() {
    if (saving_) {
        Warn("Onceki klip hala kaydediliyor");
        return;
    }
    ClipSaveInput in;
    if (!ring_.TakeSnapshot(ring_.Duration(), in.snapshot)) {
        Warn("Tampon bos, klip alinamadi");
        return;
    }
    // Disk space: the clip's size is known exactly (it is the snapshot's packets).
    uint64_t bytes = 0;
    for (const auto& p : in.snapshot.video) bytes += p.data->size();
    for (const auto& track : in.snapshot.audio)
        for (const auto& p : track) bytes += p.data->size();
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(config_.dir.c_str(), &freeBytes, nullptr, nullptr) &&
        freeBytes.QuadPart < bytes + 200ull * 1024 * 1024) {
        Warn("Diskte yer yok: klip %.0f MB, bos %.0f MB", bytes / 1048576.0, freeBytes.QuadPart / 1048576.0);
        if (config_.onEvent) config_.onEvent(SessionEvent::DiskFull, config_.dir, 0);
        return;
    }
    in.path = ClipPath(config_.dir, config_.name);
    in.videoType = video_.OutputTypeForMux();
    for (int t = 0; t < kTrackCount; ++t) {
        in.audioTypes.push_back(aac_.OutputTypeForMux(static_cast<UINT>(t)));
        in.audioTrackNames.push_back(TrackName(static_cast<Track>(t)));
    }
    if (saver_.joinable()) saver_.join();
    saving_ = true;
    saver_ = std::thread([this, in = std::move(in)]() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const int64_t t0 = QpcNow100ns();
        try {
            const auto r = SaveClip(in);
            Log("Klip kaydedildi: %s (%.1f sn, %.1f MB, %.2f sn surdu)%s", ToUtf8(in.path).c_str(), r.seconds,
                r.bytes / (1024.0 * 1024.0), (QpcNow100ns() - t0) / 1e7, r.namesWritten ? "" : " [iz isimleri yazilamadi]");
            if (config_.onEvent) config_.onEvent(SessionEvent::ClipSaved, in.path, r.seconds);
        } catch (const HrError& e) {
            Warn("Klip kaydedilemedi: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
            if (config_.onEvent) config_.onEvent(SessionEvent::ClipFailed, in.path, 0);
        } catch (const std::exception& e) {
            Warn("Klip kaydedilemedi: %s", e.what());
            if (config_.onEvent) config_.onEvent(SessionEvent::ClipFailed, in.path, 0);
        }
        saving_ = false;
    });
}

void RecordingSession::OnVideoPacket(Packet&& p) {
    std::lock_guard lock(tapMutex_);
    if (recorder_) recorder_->PushVideo(p);
    ring_.PushVideo(std::move(p));
}

void RecordingSession::OnAudioPacket(UINT track, Packet&& p) {
    std::lock_guard lock(tapMutex_);
    if (recorder_) recorder_->PushAudio(track, p);
    ring_.PushAudio(track, std::move(p));
}

void RecordingSession::StartRecording() {
    std::lock_guard lock(tapMutex_);
    if (recorder_) return;

    RecorderConfig rc;
    rc.basePath = ClipPath(config_.dir, config_.name + L"__kayit");
    rc.basePath.resize(rc.basePath.size() - 4);  // drop ".mp4"
    rc.videoType = video_.OutputTypeForMux();
    for (int t = 0; t < kTrackCount; ++t) {
        rc.audioTypes.push_back(aac_.OutputTypeForMux(static_cast<UINT>(t)));
        rc.audioTrackNames.push_back(TrackName(static_cast<Track>(t)));
    }
    rc.splitMinutes = config_.splitMinutes;
    rc.onFileReady = [cb = config_.onEvent](const std::wstring& path) {
        if (cb) cb(SessionEvent::RecordingReady, path, 0);
    };

    auto recorder = std::make_unique<ManualRecorder>();
    recorder->Start(std::move(rc));
    // Start at the newest keyframe already in the buffer (at most ~1 s before the press).
    ReplayRing::Snapshot seed;
    if (ring_.TakeSnapshot(1, seed, false)) {
        for (const auto& p : seed.video) recorder->PushVideo(p);
        for (size_t t = 0; t < seed.audio.size(); ++t)
            for (const auto& p : seed.audio[t]) recorder->PushAudio(static_cast<UINT>(t), p);
    }
    recorder_ = std::move(recorder);
    Log("Kayit basladi (manuel)");
    if (config_.onEvent) config_.onEvent(SessionEvent::RecordingStarted, {}, 0);
}

void RecordingSession::StopRecording() {
    std::unique_ptr<ManualRecorder> recorder;
    {
        std::lock_guard lock(tapMutex_);
        recorder = std::move(recorder_);
    }
    if (!recorder) return;
    recorder->Stop();  // closes the last part; conversion continues in the background
    Log("Kayit durdu (manuel): %.1f sn, %d dosya", recorder->Seconds(), recorder->FileCount());
    if (config_.onEvent) config_.onEvent(SessionEvent::RecordingStopped, {}, recorder->Seconds());
}

void RecordingSession::CheckRecorderHealth() {
    bool failed = false, diskFull = false;
    {
        std::lock_guard lock(tapMutex_);
        if (recorder_ && recorder_->Failed()) {
            failed = true;
            diskFull = recorder_->DiskFull();
        }
    }
    if (!failed) return;
    StopRecording();
    if (config_.onEvent)
        config_.onEvent(diskFull ? SessionEvent::DiskFull : SessionEvent::RecordingFailed, config_.dir, 0);
}

double RecordingSession::RecordingSeconds() {
    std::lock_guard lock(tapMutex_);
    return recorder_ ? recorder_->Seconds() : 0.0;
}

void RecordingSession::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (saver_.joinable()) saver_.join();
    if (capture_) capture_->Stop();
    audio_.StopAt(static_cast<int64_t>(frames_) * kSampleRate / config_.fps);
    video_.Shutdown();
    StopRecording();  // after the encoders flushed, so the recording gets every packet
}

}  // namespace wl
