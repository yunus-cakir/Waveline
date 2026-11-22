#pragma once
#include "audio.h"
#include "common.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <mutex>
#include <vector>

namespace wl {

struct WriterConfig {
    std::wstring path;
    UINT width = 1920;
    UINT height = 1080;
    UINT fps = 60;
    UINT videoKbps = 15000;
    bool hevc = true;
    UINT audioTracks = 5;   // Mix, Game, Chat, Media, Mic
    UINT audioKbps = 192;   // AAC encoder accepts 96/128/160/192
};

// M0 muxer: Media Foundation sink writer -> MP4. Video goes through the NVIDIA hardware
// encoder MFT (NVENC) fed with D3D11 NV12 textures; audio through the built-in AAC MFT.
// M1 replaces this with our own encoder loop + packet ring buffer + muxer.
class ClipWriter : public IAudioSink {
public:
    void Open(const WriterConfig& config, IMFDXGIDeviceManager* deviceManager);
    void WriteVideo(IMFSample* sample, int64_t timeHns, int64_t durationHns);
    void WriteAudio(UINT track, const int16_t* pcm, uint32_t frames, int64_t timeHns) override;
    void Finalize();

    const std::string& VideoEncoderName() const { return encoderName_; }
    bool VideoEncoderIsHardware() const { return encoderIsHardware_; }
    double MaxVideoWriteMs() const { return maxVideoWriteMs_; }
    double AvgVideoWriteMs() const { return videoWrites_ ? totalVideoWriteMs_ / videoWrites_ : 0.0; }

private:
    void ProbeEncoder();

    WriterConfig config_;
    com_ptr<IMFSinkWriter> writer_;
    DWORD videoStream_ = 0;
    std::vector<DWORD> audioStreams_;
    std::mutex mutex_;
    std::string encoderName_ = "(bilinmiyor)";
    bool encoderIsHardware_ = false;
    double maxVideoWriteMs_ = 0, totalVideoWriteMs_ = 0;
    uint64_t videoWrites_ = 0;
};

}  // namespace wl
