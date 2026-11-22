#pragma once
#include "audio.h"
#include "packet.h"

#include <mfapi.h>
#include <mftransform.h>

#include <array>
#include <functional>
#include <mutex>

namespace wl {

// Encodes PCM to AAC-LC (Microsoft AAC encoder MFT, synchronous) for every track and
// emits packets. Plugs into AudioEngine as its sink.
class AacEncoderBank : public IAudioSink {
public:
    using PacketCallback = std::function<void(UINT track, Packet&&)>;

    void Init(UINT tracks, UINT kbps, PacketCallback onPacket);
    void WriteAudio(UINT track, const int16_t* pcm, uint32_t frames, int64_t timeHns) override;

    // Media type for the muxer (includes the AudioSpecificConfig in MF_MT_USER_DATA).
    com_ptr<IMFMediaType> OutputTypeForMux(UINT track);

private:
    struct Encoder {
        com_ptr<IMFTransform> mft;
        com_ptr<IMFMediaType> outputType;
        DWORD outputSize = 0;
    };
    void Drain(UINT track, Encoder& e);

    std::vector<Encoder> encoders_;
    PacketCallback onPacket_;
};

}  // namespace wl
