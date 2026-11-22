#include "writer.h"

#include <codecapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <strmif.h>

#include <algorithm>
#include <cstring>

namespace wl {

namespace {

com_ptr<IMFMediaType> MakeVideoType(const GUID& subtype, const WriterConfig& c) {
    com_ptr<IMFMediaType> t;
    Check(MFCreateMediaType(t.put()), "MFCreateMediaType");
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t.get(), MF_MT_FRAME_SIZE, c.width, c.height);
    MFSetAttributeRatio(t.get(), MF_MT_FRAME_RATE, c.fps, 1);
    MFSetAttributeRatio(t.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return t;
}

com_ptr<IMFMediaType> MakeAacType(UINT kbps) {
    com_ptr<IMFMediaType> t;
    Check(MFCreateMediaType(t.put()), "MFCreateMediaType");
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kbps * 1000 / 8);
    return t;
}

com_ptr<IMFMediaType> MakePcmType() {
    com_ptr<IMFMediaType> t;
    Check(MFCreateMediaType(t.put()), "MFCreateMediaType");
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * kChannels * 2);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    return t;
}

}  // namespace

void ClipWriter::Open(const WriterConfig& config, IMFDXGIDeviceManager* deviceManager) {
    config_ = config;

    com_ptr<IMFAttributes> attrs;
    Check(MFCreateAttributes(attrs.put(), 4), "MFCreateAttributes");
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, deviceManager);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    Check(MFCreateSinkWriterFromURL(config.path.c_str(), nullptr, attrs.get(), writer_.put()),
          "MFCreateSinkWriterFromURL");

    // --- video ---
    auto out = MakeVideoType(config.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, config);
    out->SetUINT32(MF_MT_AVG_BITRATE, config.videoKbps * 1000);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, config.hevc ? static_cast<UINT32>(eAVEncH265VProfile_Main_420_8)
                                                    : static_cast<UINT32>(eAVEncH264VProfile_High));
    Check(writer_->AddStream(out.get(), &videoStream_), "AddStream(video)");

    auto in = MakeVideoType(MFVideoFormat_NV12, config);

    // Encoder settings: 1 s GOP so clips can be cut on any second, no B-frames (simpler
    // timestamps for the ring buffer in M1), peak-constrained VBR.
    com_ptr<IMFAttributes> enc;
    Check(MFCreateAttributes(enc.put(), 6), "MFCreateAttributes");
    enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
    enc->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, config.videoKbps * 1000);
    enc->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, config.videoKbps * 1500);
    enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, config.fps);
    enc->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    enc->SetUINT32(CODECAPI_AVLowLatencyMode, FALSE);
    const HRESULT hr = writer_->SetInputMediaType(videoStream_, in.get(), enc.get());
    if (FAILED(hr)) {
        // Some encoder MFTs reject one of the settings; fall back to their defaults so the
        // prototype still produces a file, and report it.
        Warn("Encoder ayarlari reddedildi (0x%08X), varsayilan ayarlarla deneniyor", static_cast<unsigned>(hr));
        Check(writer_->SetInputMediaType(videoStream_, in.get(), nullptr), "SetInputMediaType(video)");
    }
    ProbeEncoder();

    // --- audio ---
    audioStreams_.clear();
    auto pcm = MakePcmType();
    for (UINT i = 0; i < config.audioTracks; ++i) {
        DWORD stream = 0;
        auto aac = MakeAacType(config.audioKbps);
        Check(writer_->AddStream(aac.get(), &stream), "AddStream(audio)");
        Check(writer_->SetInputMediaType(stream, pcm.get(), nullptr), "SetInputMediaType(audio)");
        audioStreams_.push_back(stream);
    }

    Check(writer_->BeginWriting(), "BeginWriting");
}

void ClipWriter::ProbeEncoder() {
    com_ptr<IMFTransform> transform;
    if (FAILED(writer_->GetServiceForStream(videoStream_, GUID_NULL, IID_PPV_ARGS(transform.put()))) || !transform)
        return;
    com_ptr<IMFAttributes> attrs;
    if (FAILED(transform->GetAttributes(attrs.put())) || !attrs) return;

    WCHAR* name = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(attrs->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
        encoderName_ = ToUtf8(name);
        CoTaskMemFree(name);
    }
    UINT32 urlLen = 0;
    encoderIsHardware_ = SUCCEEDED(attrs->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &urlLen));
}

void ClipWriter::WriteVideo(IMFSample* sample, int64_t timeHns, int64_t durationHns) {
    sample->SetSampleTime(timeHns);
    sample->SetSampleDuration(durationHns);
    const int64_t t0 = QpcNow100ns();
    {
        std::lock_guard lock(mutex_);
        Check(writer_->WriteSample(videoStream_, sample), "WriteSample(video)");
    }
    const double ms = (QpcNow100ns() - t0) / 10000.0;
    maxVideoWriteMs_ = std::max(maxVideoWriteMs_, ms);
    totalVideoWriteMs_ += ms;
    ++videoWrites_;
}

void ClipWriter::WriteAudio(UINT track, const int16_t* pcm, uint32_t frames, int64_t timeHns) {
    if (track >= audioStreams_.size() || frames == 0) return;
    const DWORD bytes = frames * kChannels * sizeof(int16_t);

    com_ptr<IMFMediaBuffer> buffer;
    Check(MFCreateMemoryBuffer(bytes, buffer.put()), "MFCreateMemoryBuffer");
    BYTE* dst = nullptr;
    Check(buffer->Lock(&dst, nullptr, nullptr), "IMFMediaBuffer::Lock");
    std::memcpy(dst, pcm, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);

    com_ptr<IMFSample> sample;
    Check(MFCreateSample(sample.put()), "MFCreateSample");
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(timeHns);
    sample->SetSampleDuration(FramesToHns(frames));

    std::lock_guard lock(mutex_);
    Check(writer_->WriteSample(audioStreams_[track], sample.get()), "WriteSample(audio)");
}

void ClipWriter::Finalize() {
    std::lock_guard lock(mutex_);
    if (writer_) {
        Check(writer_->Finalize(), "IMFSinkWriter::Finalize");
        writer_ = nullptr;
    }
}

}  // namespace wl
