#include "video_encoder.h"

#include <codecapi.h>
#include <mferror.h>
#include <strmif.h>

#include <chrono>

namespace wl {

namespace {

void SetCodecValue(ICodecAPI* api, const GUID& key, UINT32 value, const char* label) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    const HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr)) Warn("Encoder ayari uygulanamadi: %s (0x%08X)", label, static_cast<unsigned>(hr));
}

com_ptr<IMFMediaType> MakeType(const GUID& subtype, const VideoEncoderConfig& c) {
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

}  // namespace

std::vector<uint8_t> ExtractParameterSets(const std::vector<uint8_t>& b, bool hevc) {
    std::vector<uint8_t> out;
    const size_t n = b.size();
    size_t i = 0;
    auto startCodeAt = [&](size_t p) -> size_t {  // length of start code at p, 0 if none
        if (p + 3 <= n && b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 1) return 3;
        if (p + 4 <= n && b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 0 && b[p + 3] == 1) return 4;
        return 0;
    };
    while (i < n) {
        const size_t sc = startCodeAt(i);
        if (!sc) { ++i; continue; }
        const size_t nalStart = i + sc;
        size_t j = nalStart;
        while (j < n && !startCodeAt(j)) ++j;
        if (nalStart < j) {
            const uint8_t h = b[nalStart];
            const int type = hevc ? (h >> 1) & 0x3F : h & 0x1F;
            const bool isParam = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
            if (isParam) {
                out.insert(out.end(), {0, 0, 0, 1});
                out.insert(out.end(), b.begin() + static_cast<ptrdiff_t>(nalStart), b.begin() + static_cast<ptrdiff_t>(j));
            }
        }
        i = j;
    }
    return out;
}

VideoEncoder::~VideoEncoder() {
    try {
        Shutdown();
    } catch (...) {
    }
}

void VideoEncoder::Init(const VideoEncoderConfig& config, IMFDXGIDeviceManager* deviceManager, PacketCallback onPacket) {
    config_ = config;
    onPacket_ = std::move(onPacket);
    const GUID outSubtype = config.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;

    // --- find the hardware encoder (prefer NVIDIA) ---
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, outSubtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    Check(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inInfo,
                    &outInfo, &activates, &count),
          "MFTEnumEx(video encoder)");
    int pick = -1;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        UINT32 len = 0;
        std::string n;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
            n = ToUtf8(name);
            CoTaskMemFree(name);
        }
        if (pick < 0 || n.find("NVIDIA") != std::string::npos) {
            pick = static_cast<int>(i);
            name_ = n;
        }
    }
    if (pick >= 0) {
        const HRESULT hr = activates[pick]->ActivateObject(IID_PPV_ARGS(mft_.put()));
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        Check(hr, "ActivateObject(video encoder)");
    } else {
        CoTaskMemFree(activates);
        throw std::runtime_error("Donanim video encoder'i bulunamadi");
    }

    // --- async unlock + GPU device ---
    com_ptr<IMFAttributes> attrs;
    Check(mft_->GetAttributes(attrs.put()), "IMFTransform::GetAttributes");
    Check(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF_TRANSFORM_ASYNC_UNLOCK");
    Check(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager)),
          "MFT_MESSAGE_SET_D3D_MANAGER");

    // --- settings, then types (output first for encoders) ---
    if (auto api = mft_.try_as<ICodecAPI>()) {
        SetCodecValue(api.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR, "rate control");
        SetCodecValue(api.get(), CODECAPI_AVEncCommonMeanBitRate, config.kbps * 1000, "mean bitrate");
        SetCodecValue(api.get(), CODECAPI_AVEncCommonMaxBitRate, config.kbps * 1500, "max bitrate");
        SetCodecValue(api.get(), CODECAPI_AVEncMPVGOPSize, config.fps, "GOP");
        SetCodecValue(api.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, "B-frames");
    }
    auto out = MakeType(outSubtype, config);
    out->SetUINT32(MF_MT_AVG_BITRATE, config.kbps * 1000);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, config.hevc ? static_cast<UINT32>(eAVEncH265VProfile_Main_420_8)
                                                    : static_cast<UINT32>(eAVEncH264VProfile_High));
    Check(mft_->SetOutputType(0, out.get(), 0), "SetOutputType(video)");
    auto in = MakeType(MFVideoFormat_NV12, config);
    Check(mft_->SetInputType(0, in.get(), 0), "SetInputType(video)");

    MFT_OUTPUT_STREAM_INFO info{};
    Check(mft_->GetOutputStreamInfo(0, &info), "GetOutputStreamInfo");
    providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputBufferSize_ = info.cbSize ? info.cbSize : config.width * config.height * 2;

    events_ = mft_.as<IMFMediaEventGenerator>();
    Check(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "MFT flush");
    Check(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MFT begin streaming");
    Check(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MFT start of stream");
    thread_ = std::thread(&VideoEncoder::EventLoop, this);
}

bool VideoEncoder::Encode(IMFSample* sample, int64_t timeHns, int64_t durationHns) {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_ || needInput_ <= 0) {
            ++skipped_;
            return false;
        }
        --needInput_;
    }
    sample->SetSampleTime(timeHns);
    sample->SetSampleDuration(durationHns);
    Check(mft_->ProcessInput(0, sample, 0), "IMFTransform::ProcessInput(video)");
    return true;
}

void VideoEncoder::EventLoop() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    for (;;) {
        com_ptr<IMFMediaEvent> ev;
        const HRESULT hr = events_->GetEvent(0, ev.put());  // blocks
        if (FAILED(hr)) break;                              // MF_E_SHUTDOWN
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        try {
            if (type == METransformNeedInput) {
                std::lock_guard lock(mutex_);
                ++needInput_;
            } else if (type == METransformHaveOutput) {
                DrainOutput();
            } else if (type == METransformDrainComplete) {
                std::lock_guard lock(mutex_);
                drainDone_ = true;
                drained_.notify_all();
            }
        } catch (const HrError& e) {
            Warn("Video encoder cikisi islenemedi: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
        }
    }
}

void VideoEncoder::DrainOutput() {
    MFT_OUTPUT_DATA_BUFFER out{};
    com_ptr<IMFSample> own;
    if (!providesSamples_) {
        com_ptr<IMFMediaBuffer> buffer;
        Check(MFCreateMemoryBuffer(outputBufferSize_, buffer.put()), "MFCreateMemoryBuffer");
        Check(MFCreateSample(own.put()), "MFCreateSample");
        own->AddBuffer(buffer.get());
        out.pSample = own.get();
    }
    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
    if (out.pEvents) out.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        com_ptr<IMFMediaType> t;
        if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, t.put()))) mft_->SetOutputType(0, t.get(), 0);
        return;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
    Check(hr, "IMFTransform::ProcessOutput(video)");

    com_ptr<IMFSample> sample;
    if (providesSamples_) sample.attach(out.pSample);  // we own the MFT's sample now
    else sample = own;
    if (!sample) return;

    com_ptr<IMFMediaBuffer> contiguous;
    Check(sample->ConvertToContiguousBuffer(contiguous.put()), "ConvertToContiguousBuffer");
    BYTE* bytes = nullptr;
    DWORD length = 0;
    Check(contiguous->Lock(&bytes, nullptr, &length), "Lock(video packet)");
    auto data = std::make_shared<std::vector<uint8_t>>(bytes, bytes + length);
    contiguous->Unlock();

    Packet p;
    sample->GetSampleTime(&p.pts);
    if (FAILED(sample->GetSampleDuration(&p.duration)) || p.duration <= 0) p.duration = kHns / config_.fps;
    p.keyframe = MFGetAttributeUINT32(sample.get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;
    if (p.keyframe) CaptureSequenceHeader(*data);
    p.data = std::move(data);
    ++encoded_;
    if (onPacket_) onPacket_(std::move(p));
}

void VideoEncoder::CaptureSequenceHeader(const std::vector<uint8_t>& keyframe) {
    std::lock_guard lock(mutex_);
    if (!sequenceHeader_.empty()) return;
    // Prefer the header the encoder publishes on its output type; fall back to parsing.
    com_ptr<IMFMediaType> current;
    if (SUCCEEDED(mft_->GetOutputCurrentType(0, current.put()))) {
        UINT32 size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
            sequenceHeader_.resize(size);
            current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), size, nullptr);
            return;
        }
    }
    sequenceHeader_ = ExtractParameterSets(keyframe, config_.hevc);
}

com_ptr<IMFMediaType> VideoEncoder::OutputTypeForMux() {
    auto t = MakeType(config_.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, config_);
    t->SetUINT32(MF_MT_AVG_BITRATE, config_.kbps * 1000);
    std::lock_guard lock(mutex_);
    if (!sequenceHeader_.empty())
        t->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), static_cast<UINT32>(sequenceHeader_.size()));
    return t;
}

void VideoEncoder::Shutdown() {
    if (!mft_) return;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        shutdown_ = true;
    }
    if (SUCCEEDED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0)) &&
        SUCCEEDED(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) {
        std::unique_lock lock(mutex_);
        drained_.wait_for(lock, std::chrono::seconds(2), [this] { return drainDone_; });
    }
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    if (auto sd = mft_.try_as<IMFShutdown>()) sd->Shutdown();  // unblocks GetEvent
    if (thread_.joinable()) thread_.join();
    events_ = nullptr;
    mft_ = nullptr;
}

}  // namespace wl
