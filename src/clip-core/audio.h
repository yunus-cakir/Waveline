#pragma once
#include "common.h"

#include <mmdeviceapi.h>
#include <audioclient.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace wl {

// Receives the aligned PCM of every track (48 kHz, stereo, 16-bit), e.g. a file writer or
// the AAC encoders of the replay buffer.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    virtual void WriteAudio(UINT track, const int16_t* pcm, uint32_t frames, int64_t timeHns) = 0;
};

// Track layout decided for Waveline Clip: 1 Mix, 2 Game, 3 Chat, 4 Media, 5 Mic.
enum class Track : int { Mix = 0, Game, Chat, Media, Mic, Count };
constexpr int kTrackCount = static_cast<int>(Track::Count);
const char* TrackName(Track t);

// Roles written by the SteelSeries Sonar VAD driver into each endpoint's property store
// (PKEY {F141E7CC-921B-466D-802F-879D6D4AEC17},0). Stable across languages/renames.
enum SonarRole : int { kSonarNone = -1, kSonarChat = 1, kSonarGame = 2, kSonarMic = 3,
                       kSonarStream = 4, kSonarMedia = 5, kSonarAux = 6 };

struct EndpointInfo {
    std::wstring id;
    std::wstring name;
    bool capture = false;
    int sonarRole = kSonarNone;
};

std::vector<EndpointInfo> EnumerateEndpoints();
std::wstring DefaultEndpointId(bool capture);

struct SourceStats {
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t silentPackets = 0;
    uint64_t discontinuities = 0;
    uint64_t gapFilledFrames = 0;       // holes between packets filled with silence
    uint64_t overlapDroppedFrames = 0;  // late/overlapping data dropped
    uint64_t deadlineFilledFrames = 0;  // no packet in time (silent loopback) -> silence
    uint64_t reconnects = 0;
    bool deviceLost = false;
};

// One WASAPI shared-mode stream (loopback for render endpoints, normal for microphones).
// Packets are placed on an absolute timeline using their QPC timestamps; holes are filled
// with silence and overlaps are trimmed, so every track stays sample-aligned with video.
class AudioSource {
public:
    AudioSource(std::wstring deviceId, std::string label, bool loopback);
    ~AudioSource();
    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;

    void Open();                     // initialise the device (throws on failure)
    // Finds the device again after it disappeared (by Sonar role), so reconnects survive
    // a changed endpoint id. Without it the original id is retried.
    void SetResolver(std::function<std::wstring()> resolver) { resolver_ = std::move(resolver); }
    void Start(int64_t startHns);    // timeline frame 0 == startHns
    void Stop();

    // Makes sure data exists up to `untilFrame` (silence if nothing arrived) and moves the
    // frames [readPos, untilFrame) into `out` as interleaved float stereo.
    void ReadUntil(int64_t untilFrame, std::vector<float>& out);

    SourceStats Stats();
    const std::string& Label() const { return label_; }
    bool Loopback() const { return loopback_; }

private:
    void Run();
    bool TryReopen();
    void Push(int64_t pos, const float* data, uint32_t frames);
    std::function<std::wstring()> resolver_;
    int64_t WritePosLocked() const { return fifoPos_ + static_cast<int64_t>(fifo_.size() / kChannels); }

    std::wstring deviceId_;
    std::string label_;
    bool loopback_;

    com_ptr<IAudioClient> client_;
    com_ptr<IAudioCaptureClient> capture_;
    HANDLE event_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int64_t startHns_ = 0;

    std::mutex mutex_;
    std::vector<float> fifo_;  // interleaved stereo, fifo_[0] is frame fifoPos_
    int64_t fifoPos_ = 0;
    SourceStats stats_;
};

// Pulls all sources on a fixed cadence, builds the Mix track and hands PCM to the writer.
class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // `source` may be null: the track is then written as silence (layout stays the same).
    void SetSource(Track track, std::unique_ptr<AudioSource> source);
    AudioSource* Source(Track track) const { return sources_[static_cast<int>(track)].get(); }

    void Start(IAudioSink* sink, int64_t startHns);
    // Flushes audio up to `endFrame` (matching the video length) and stops.
    void StopAt(int64_t endFrame);

    uint64_t MixClippedSamples() const { return mixClipped_; }
    bool Failed() const { return failed_; }

private:
    void Run();
    void RunLoop();
    std::atomic<bool> failed_{false};
    std::atomic<bool> abort_{false};
    void Process(int64_t untilFrame);

    std::array<std::unique_ptr<AudioSource>, kTrackCount> sources_;
    IAudioSink* writer_ = nullptr;
    int64_t startHns_ = 0;
    int64_t mixPos_ = 0;
    std::thread thread_;
    std::atomic<int64_t> stopFrame_{-1};
    std::array<std::vector<float>, kTrackCount> buffers_;
    std::vector<int16_t> pcm_;
    uint64_t mixClipped_ = 0;
};

}  // namespace wl
