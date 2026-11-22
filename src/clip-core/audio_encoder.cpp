#include "audio_encoder.h"

#include <mferror.h>
#include <wmcodecdsp.h>

#include <cstring>

namespace wl {

void AacEncoderBank::Init(UINT tracks, UINT kbps, PacketCallback onPacket) {
    onPacket_ = std::move(onPacket);
    encoders_.resize(tracks);
    for (auto& e : encoders_) {
        Check(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(e.mft.put())),
              "CoCreateInstance(AAC encoder)");

        com_ptr<IMFMediaType> out;
        Check(MFCreateMediaType(out.put()), "MFCreateMediaType");
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kbps * 1000 / 8);
        out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);  // raw AAC (what MP4 stores)
        Check(e.mft->SetOutputType(0, out.get(), 0), "SetOutputType(AAC)");

        com_ptr<IMFMediaType> in;
        Check(MFCreateMediaType(in.put()), "MFCreateMediaType");
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * kChannels * 2);
        Check(e.mft->SetInputType(0, in.get(), 0), "SetInputType(AAC)");

        MFT_OUTPUT_STREAM_INFO info{};
        Check(e.mft->GetOutputStreamInfo(0, &info), "GetOutputStreamInfo(AAC)");
        e.outputSize = info.cbSize ? info.cbSize : 8192;
        // Cached now: the save thread must not call into an MFT the audio thread is using.
        Check(e.mft->GetOutputCurrentType(0, e.outputType.put()), "GetOutputCurrentType(AAC)");
        e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
}

void AacEncoderBank::WriteAudio(UINT track, const int16_t* pcm, uint32_t frames, int64_t timeHns) {
    if (track >= encoders_.size() || frames == 0) return;
    auto& e = encoders_[track];
    const DWORD bytes = frames * kChannels * sizeof(int16_t);

    com_ptr<IMFMediaBuffer> buffer;
    Check(MFCreateMemoryBuffer(bytes, buffer.put()), "MFCreateMemoryBuffer");
    BYTE* dst = nullptr;
    Check(buffer->Lock(&dst, nullptr, nullptr), "Lock(pcm)");
    std::memcpy(dst, pcm, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    com_ptr<IMFSample> sample;
    Check(MFCreateSample(sample.put()), "MFCreateSample");
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(timeHns);
    sample->SetSampleDuration(FramesToHns(frames));

    HRESULT hr = e.mft->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        Drain(track, e);
        hr = e.mft->ProcessInput(0, sample.get(), 0);
    }
    Check(hr, "ProcessInput(AAC)");
    Drain(track, e);
}

void AacEncoderBank::Drain(UINT track, Encoder& e) {
    for (;;) {
        com_ptr<IMFMediaBuffer> buffer;
        Check(MFCreateMemoryBuffer(e.outputSize, buffer.put()), "MFCreateMemoryBuffer");
        com_ptr<IMFSample> sample;
        Check(MFCreateSample(sample.put()), "MFCreateSample");
        sample->AddBuffer(buffer.get());

        MFT_OUTPUT_DATA_BUFFER out{};
        out.pSample = sample.get();
        DWORD status = 0;
        const HRESULT hr = e.mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        Check(hr, "ProcessOutput(AAC)");

        BYTE* bytes = nullptr;
        DWORD length = 0;
        Check(buffer->Lock(&bytes, nullptr, &length), "Lock(aac)");
        Packet p;
        p.data = std::make_shared<std::vector<uint8_t>>(bytes, bytes + length);
        buffer->Unlock();
        sample->GetSampleTime(&p.pts);
        if (FAILED(sample->GetSampleDuration(&p.duration)) || p.duration <= 0) p.duration = FramesToHns(1024);
        p.keyframe = true;
        if (length > 0 && onPacket_) onPacket_(track, std::move(p));
    }
}

com_ptr<IMFMediaType> AacEncoderBank::OutputTypeForMux(UINT track) {
    return encoders_.at(track).outputType;
}

}  // namespace wl
