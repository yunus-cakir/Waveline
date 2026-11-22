#pragma once
#include "packet.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace wl {

struct VideoEncoderConfig {
    UINT width = 1920;
    UINT height = 1080;
    UINT fps = 60;
    UINT kbps = 15000;
    bool hevc = true;
};

// Drives the NVIDIA hardware encoder MFT (NVENC) directly, in its asynchronous mode.
// Input: NV12 D3D11 samples. Output: compressed packets via the callback, on the MFT's
// event thread. One GOP per second, no B-frames, so every second is a clean cut point.
class VideoEncoder {
public:
    using PacketCallback = std::function<void(Packet&&)>;

    ~VideoEncoder();
    void Init(const VideoEncoderConfig& config, IMFDXGIDeviceManager* deviceManager, PacketCallback onPacket);

    // Returns false when the encoder has no free input slot (it is behind): the frame is
    // skipped instead of blocking the capture loop.
    bool Encode(IMFSample* sample, int64_t timeHns, int64_t durationHns);

    // Flushes remaining frames and stops the event thread.
    void Shutdown();

    // Media type of the compressed stream for the muxer, including the codec headers
    // (VPS/SPS/PPS). Valid after the first packet.
    com_ptr<IMFMediaType> OutputTypeForMux();

    const std::string& Name() const { return name_; }
    uint64_t SkippedFrames() const { return skipped_; }
    uint64_t EncodedFrames() const { return encoded_; }

private:
    void EventLoop();
    void DrainOutput();
    void CaptureSequenceHeader(const std::vector<uint8_t>& keyframe);

    VideoEncoderConfig config_;
    com_ptr<IMFTransform> mft_;
    com_ptr<IMFMediaEventGenerator> events_;
    PacketCallback onPacket_;
    std::string name_;
    DWORD outputBufferSize_ = 0;
    bool providesSamples_ = true;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable drained_;
    int needInput_ = 0;
    bool drainDone_ = false;
    bool shutdown_ = false;
    std::atomic<uint64_t> skipped_{0}, encoded_{0};
    std::vector<uint8_t> sequenceHeader_;
};

// Extracts the parameter sets (H.264: SPS/PPS, HEVC: VPS/SPS/PPS) from an Annex-B
// keyframe, returned as Annex-B (start code + NAL) concatenation.
std::vector<uint8_t> ExtractParameterSets(const std::vector<uint8_t>& annexB, bool hevc);

}  // namespace wl
