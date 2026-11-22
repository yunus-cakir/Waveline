#include "manual_recorder.h"
#include "background.h"
#include "clip_saver.h"

#include <mferror.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace wl {

namespace {

com_ptr<IMFSample> MakeSample(const Packet& p, int64_t rebase) {
    const DWORD size = static_cast<DWORD>(p.data->size());
    com_ptr<IMFMediaBuffer> buffer;
    Check(MFCreateMemoryBuffer(size, buffer.put()), "MFCreateMemoryBuffer");
    BYTE* dst = nullptr;
    Check(buffer->Lock(&dst, nullptr, nullptr), "Lock");
    std::memcpy(dst, p.data->data(), size);
    buffer->Unlock();
    buffer->SetCurrentLength(size);
    com_ptr<IMFSample> s;
    Check(MFCreateSample(s.put()), "MFCreateSample");
    s->AddBuffer(buffer.get());
    s->SetSampleTime(p.pts - rebase);
    s->SetSampleDuration(p.duration);
    if (p.keyframe) s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    return s;
}

}  // namespace

ManualRecorder::~ManualRecorder() {
    try {
        Stop();
    } catch (...) {
    }
}

void ManualRecorder::Start(RecorderConfig config) {
    config_ = std::move(config);
    const size_t streams = 1 + config_.audioTypes.size();
    queues_.assign(streams, {});
    lastPts_.assign(streams, INT64_MIN);
    stopping_ = false;
    thread_ = std::thread(&ManualRecorder::WriterLoop, this);
}

void ManualRecorder::PushVideo(const Packet& p) {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    queues_[0].push_back(p);
    lastPts_[0] = p.pts;
    cv_.notify_one();
}

void ManualRecorder::PushAudio(UINT track, const Packet& p) {
    std::lock_guard lock(mutex_);
    if (stopping_ || track + 1 >= queues_.size()) return;
    queues_[track + 1].push_back(p);
    lastPts_[track + 1] = p.pts;
}

void ManualRecorder::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ && !thread_.joinable()) return;
        stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

// Packets arrive from different threads with different latency (audio ~150 ms behind
// video). Everything up to the newest timestamp that *all* streams have reached is written
// in timestamp order, so the file is properly interleaved.
void ManualRecorder::WriterLoop() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<std::pair<int, Packet>> batch;
    bool done = false;
    try {
        while (!done) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100));
                int64_t limit = INT64_MAX;
                if (!stopping_) {
                    for (int64_t t : lastPts_) limit = std::min(limit, t);
                } else {
                    // Final flush: end where the audio ends (it lags video by ~150 ms), so the
                    // last file has no silent video tail.
                    for (size_t s = 1; s < lastPts_.size(); ++s) limit = std::min(limit, lastPts_[s]);
                }
                done = stopping_;
                for (;;) {
                    int best = -1;
                    int64_t bestPts = INT64_MAX;
                    for (size_t s = 0; s < queues_.size(); ++s)
                        if (!queues_[s].empty() && queues_[s].front().pts < bestPts) {
                            bestPts = queues_[s].front().pts;
                            best = static_cast<int>(s);
                        }
                    if (best < 0 || bestPts > limit) break;
                    batch.emplace_back(best, std::move(queues_[best].front()));
                    queues_[best].pop_front();
                }
                if (done)
                    for (auto& q : queues_) q.clear();
            }
            for (auto& [stream, packet] : batch) WritePacket(stream, packet);
            batch.clear();
        }
    } catch (const HrError& e) {
        Warn("Kayit dosyasina yazilamadi: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
    } catch (const std::exception& e) {
        Warn("Kayit dosyasina yazilamadi: %s", e.what());
    }
    try {
        CloseSegment();
        FinishGroup();
    } catch (const HrError& e) {
        Warn("Kayit dosyasi kapatilamadi: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
    }
}

namespace {
constexpr int64_t kSegmentHns = 10 * kHns;  // crash-loss window
}

void ManualRecorder::WritePacket(int stream, const Packet& p) {
    const bool video = stream == 0;
    if (!finalWriter_) {
        if (!video || !p.keyframe) return;  // every file starts on a keyframe
        StartGroup(p.pts);
        OpenSegment(p.pts);
    } else if (video && p.keyframe) {
        if (config_.splitMinutes > 0 && p.pts - groupStartPts_ >= static_cast<int64_t>(config_.splitMinutes * 60.0 * kHns)) {
            CloseSegment();
            FinishGroup();
            StartGroup(p.pts);
            OpenSegment(p.pts);
        } else if (p.pts - segStartPts_ >= kSegmentHns) {
            CloseSegment();
            OpenSegment(p.pts);
        }
    }
    if (p.pts < segStartPts_) return;  // audio from before the first frame
    const auto s = static_cast<size_t>(stream);
    Check(finalWriter_->WriteSample(finalStreams_[s], MakeSample(p, groupStartPts_).get()), "WriteSample(kayit)");
    if (writer_) Check(writer_->WriteSample(streams_[s], MakeSample(p, segStartPts_).get()), "WriteSample(yedek)");
    if (video) {
        if (firstPts_ < 0) firstPts_ = p.pts;
        seconds_ = (p.pts + p.duration - firstPts_) / 1e7;
    }
}

namespace {
com_ptr<IMFSinkWriter> CreateMp4Writer(const std::wstring& path, const RecorderConfig& c, std::vector<DWORD>& streams) {
    com_ptr<IMFAttributes> attrs;
    Check(MFCreateAttributes(attrs.put(), 2), "MFCreateAttributes");
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    com_ptr<IMFSinkWriter> w;
    Check(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.get(), w.put()), "MFCreateSinkWriterFromURL(kayit)");
    streams.clear();
    DWORD s = 0;
    Check(w->AddStream(c.videoType.get(), &s), "AddStream(video)");
    Check(w->SetInputMediaType(s, c.videoType.get(), nullptr), "SetInputMediaType(video)");
    streams.push_back(s);
    for (const auto& t : c.audioTypes) {
        Check(w->AddStream(t.get(), &s), "AddStream(audio)");
        Check(w->SetInputMediaType(s, t.get(), nullptr), "SetInputMediaType(audio)");
        streams.push_back(s);
    }
    Check(w->BeginWriting(), "BeginWriting(kayit)");
    return w;
}
}  // namespace

void ManualRecorder::StartGroup(int64_t pts) {
    const int index = ++fileIndex_;
    const std::wstring suffix = index > 1 ? L"_" + std::to_wstring(index) : L"";
    finalPath_ = config_.basePath + suffix + L".mp4";
    partsDir_ = config_.basePath + suffix + L".parts";
    std::filesystem::create_directories(partsDir_);
    groupStartPts_ = pts;
    segmentCount_ = 0;
    finalWriter_ = CreateMp4Writer(finalPath_, config_, finalStreams_);
    Log("Kayit dosyasi: %s", ToUtf8(finalPath_).c_str());
}

void ManualRecorder::OpenSegment(int64_t pts) {
    segStartPts_ = pts;
    wchar_t name[32];
    swprintf_s(name, L"\\%05zu.mp4", ++segmentCount_);
    try {
        writer_ = CreateMp4Writer(partsDir_ + name, config_, streams_);
    } catch (const HrError& e) {  // the backup is optional; keep recording the final file
        writer_ = nullptr;
        Warn("Yedek bolum acilamadi (0x%08X)", static_cast<unsigned>(e.hr));
    }
}

void ManualRecorder::CloseSegment() {
    if (!writer_) return;
    auto writer = std::move(writer_);
    writer_ = nullptr;
    Check(writer->Finalize(), "Finalize(yedek)");
}

void ManualRecorder::FinishGroup() {
    if (!finalWriter_) return;
    auto writer = std::move(finalWriter_);
    finalWriter_ = nullptr;
    Check(writer->Finalize(), "Finalize(kayit)");
    writer = nullptr;
    // The final file is complete: drop the crash backup and name the tracks.
    std::error_code ec;
    wchar_t keep[8];
    if (!GetEnvironmentVariableW(L"WAVELINE_KEEP_PARTS", keep, 8)) std::filesystem::remove_all(partsDir_, ec);
    const bool named = Mp4SetTrackNames(finalPath_, "Video", config_.audioTrackNames);
    Log("Kayit hazir: %s%s", ToUtf8(finalPath_).c_str(), named ? "" : " [iz isimleri yazilamadi]");
    if (config_.onFileReady) config_.onFileReady(finalPath_);
}

}  // namespace wl