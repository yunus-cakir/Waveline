#pragma once
#include "audio.h"
#include "audio_encoder.h"
#include "capture.h"
#include "manual_recorder.h"
#include "replay_ring.h"
#include "video_encoder.h"
#include "videoproc.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace wl {

enum class SessionEvent { ClipSaved, ClipFailed, RecordingStarted, RecordingStopped, RecordingReady, DiskFull, RecordingFailed };

struct SessionConfig {
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    std::wstring name;  // game name ("Counter-Strike 2") or "Masaüstü": folder, file names, tags
    std::wstring dir;   // base clip folder; files go to the <dir>\<name> subfolder
    double minutes = 1.0;
    UINT fps = 60;
    UINT kbps = 15000;
    bool hevc = true;
    bool cursor = true;
    double splitMinutes = 0;  // manual recording: start a new file every N minutes (0 = off)
    // Sounds/notifications are the app's job. Called from worker threads.
    std::function<void(SessionEvent, const std::wstring& path, double seconds)> onEvent;
};

// One continuous recording into the replay buffer (one game, or the desktop):
// capture -> NV12 -> NVENC + 5 audio tracks -> ReplayRing. Runs its own video thread.
class RecordingSession {
public:
    explicit RecordingSession(SessionConfig config);
    ~RecordingSession();
    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    void Start();  // throws on failure
    void Stop();
    void RequestSave() { saveRequested_ = true; }

    // Manual recording, fed by the same encoder as the replay buffer.
    void StartRecording();
    void StopRecording();
    bool Recording() const { return recorder_ != nullptr; }
    double RecordingSeconds();

    bool Ended() const { return ended_; }  // target closed or fatal error
    const std::wstring& Name() const { return config_.name; }
    double BufferedSeconds() const { return ring_.BufferedHns() / 1e7; }
    double BufferMB() const { return ring_.Bytes() / (1024.0 * 1024.0); }
    uint64_t SkippedFrames() const { return video_.SkippedFrames(); }

private:
    void Loop();
    void SaveNow();
    void CheckRecorderHealth();
    void OnVideoPacket(Packet&& p);
    void OnAudioPacket(UINT track, Packet&& p);

    // Packets go to the ring and, while recording, to the recorder. One lock makes starting
    // a recording (seed from the ring + attach) atomic with respect to new packets.
    std::mutex tapMutex_;
    std::unique_ptr<ManualRecorder> recorder_;

    SessionConfig config_;
    std::wstring folder_;  // <dir>\<game>
    GpuDevice gpu_;
    com_ptr<IMFDXGIDeviceManager> deviceManager_;
    std::unique_ptr<CaptureSource> capture_;
    VideoConverter converter_;
    com_ptr<IMFVideoSampleAllocatorEx> allocator_;
    ReplayRing ring_{kTrackCount};
    VideoEncoder video_;
    AacEncoderBank aac_;
    AudioEngine audio_;
    UINT outW_ = 0, outH_ = 0;

    std::thread thread_;
    std::thread saver_;
    std::atomic<bool> stop_{false}, ended_{false}, saveRequested_{false}, saving_{false};
    uint64_t frames_ = 0;
};

// Picks Sonar's Game/Chat/Media/Mic endpoints (or fallbacks) for the audio engine.
void SetupAudioSources(AudioEngine& engine, bool logChoices);

}  // namespace wl
