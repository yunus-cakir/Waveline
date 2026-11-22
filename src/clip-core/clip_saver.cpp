#include "clip_saver.h"

#include <mfreadwrite.h>

#include <cstring>
#include <filesystem>
#include <fstream>

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

// ---- minimal MP4 box editing ------------------------------------------------

uint32_t ReadU32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void WriteU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

struct Box {
    std::string type;
    std::vector<uint8_t> payload;  // for leaf boxes
    std::vector<Box> children;     // for containers we edit
    bool container = false;
};

bool IsContainer(const std::string& t) { return t == "moov" || t == "trak" || t == "mdia"; }

bool ParseBoxes(const uint8_t* p, size_t n, std::vector<Box>& out) {
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t size = ReadU32(p + i);
        std::string type(reinterpret_cast<const char*>(p + i + 4), 4);
        size_t header = 8;
        if (size == 1) {  // 64-bit size
            if (i + 16 > n) return false;
            size = (uint64_t(ReadU32(p + i + 8)) << 32) | ReadU32(p + i + 12);
            header = 16;
        } else if (size == 0) {
            size = n - i;
        }
        if (size < header || i + size > n) return false;
        Box b;
        b.type = type;
        if (IsContainer(type)) {
            b.container = true;
            if (!ParseBoxes(p + i + header, static_cast<size_t>(size) - header, b.children)) return false;
        } else {
            b.payload.assign(p + i + header, p + i + size);
        }
        out.push_back(std::move(b));
        i += static_cast<size_t>(size);
    }
    return i == n;
}

void Serialize(const Box& b, std::vector<uint8_t>& out) {
    std::vector<uint8_t> body;
    if (b.container)
        for (const auto& c : b.children) Serialize(c, body);
    else
        body = b.payload;
    WriteU32(out, static_cast<uint32_t>(body.size() + 8));
    out.insert(out.end(), b.type.begin(), b.type.end());
    out.insert(out.end(), body.begin(), body.end());
}

}  // namespace

bool Mp4SetTrackNames(const std::wstring& path, const std::string& videoName, const std::vector<std::string>& audioNames) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) return false;

    // Locate top-level boxes; we only edit when moov is the last box (so no chunk offsets move).
    f.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(f.tellg());
    uint64_t pos = 0, moovPos = 0, moovSize = 0;
    while (pos + 8 <= fileSize) {
        uint8_t h[16];
        f.seekg(static_cast<std::streamoff>(pos));
        f.read(reinterpret_cast<char*>(h), 8);
        uint64_t size = ReadU32(h);
        const std::string type(reinterpret_cast<char*>(h + 4), 4);
        if (size == 1) {
            f.read(reinterpret_cast<char*>(h + 8), 8);
            size = (uint64_t(ReadU32(h + 8)) << 32) | ReadU32(h + 12);
        } else if (size == 0) {
            size = fileSize - pos;
        }
        if (size < 8) return false;
        if (type == "moov") { moovPos = pos; moovSize = size; }
        pos += size;
    }
    if (!moovSize || moovPos + moovSize != fileSize) return false;

    std::vector<uint8_t> raw(static_cast<size_t>(moovSize));
    f.seekg(static_cast<std::streamoff>(moovPos));
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    std::vector<Box> top;
    if (!ParseBoxes(raw.data(), raw.size(), top) || top.size() != 1) return false;

    size_t audioIndex = 0;
    for (auto& trak : top[0].children) {
        if (trak.type != "trak") continue;
        for (auto& mdia : trak.children) {
            if (mdia.type != "mdia") continue;
            for (auto& hdlr : mdia.children) {
                if (hdlr.type != "hdlr" || hdlr.payload.size() < 24) continue;
                const std::string handler(reinterpret_cast<const char*>(hdlr.payload.data() + 8), 4);
                std::string name;
                if (handler == "vide") name = videoName;
                else if (handler == "soun" && audioIndex < audioNames.size()) name = audioNames[audioIndex++];
                else continue;
                // version/flags(4) pre_defined(4) handler_type(4) reserved(12) name(\0-terminated)
                hdlr.payload.resize(24);
                hdlr.payload.insert(hdlr.payload.end(), name.begin(), name.end());
                hdlr.payload.push_back(0);
            }
        }
    }

    std::vector<uint8_t> rebuilt;
    Serialize(top[0], rebuilt);
    f.seekp(static_cast<std::streamoff>(moovPos));
    f.write(reinterpret_cast<const char*>(rebuilt.data()), static_cast<std::streamsize>(rebuilt.size()));
    f.close();
    std::error_code ec;
    std::filesystem::resize_file(path, moovPos + rebuilt.size(), ec);
    return !ec;
}

ClipSaveResult SaveClip(const ClipSaveInput& in) {
    const auto& snap = in.snapshot;
    com_ptr<IMFAttributes> attrs;
    Check(MFCreateAttributes(attrs.put(), 1), "MFCreateAttributes");
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    com_ptr<IMFSinkWriter> writer;
    Check(MFCreateSinkWriterFromURL(in.path.c_str(), nullptr, attrs.get(), writer.put()), "MFCreateSinkWriterFromURL");

    // Same type in and out = pass-through: packets are stored as they are.
    DWORD videoStream = 0;
    Check(writer->AddStream(in.videoType.get(), &videoStream), "AddStream(video)");
    Check(writer->SetInputMediaType(videoStream, in.videoType.get(), nullptr), "SetInputMediaType(video)");
    std::vector<DWORD> audioStreams;
    for (const auto& t : in.audioTypes) {
        DWORD s = 0;
        Check(writer->AddStream(t.get(), &s), "AddStream(audio)");
        Check(writer->SetInputMediaType(s, t.get(), nullptr), "SetInputMediaType(audio)");
        audioStreams.push_back(s);
    }
    Check(writer->BeginWriting(), "BeginWriting");

    // Interleave by timestamp so the file is written in playback order.
    const int64_t rebase = snap.startPts;
    std::vector<size_t> next(snap.audio.size(), 0);
    size_t v = 0;
    for (;;) {
        int bestTrack = -1;  // -1 = video
        int64_t bestPts = INT64_MAX;
        if (v < snap.video.size()) bestPts = snap.video[v].pts;
        for (size_t t = 0; t < snap.audio.size() && t < audioStreams.size(); ++t)
            if (next[t] < snap.audio[t].size() && snap.audio[t][next[t]].pts < bestPts) {
                bestPts = snap.audio[t][next[t]].pts;
                bestTrack = static_cast<int>(t);
            }
        if (bestPts == INT64_MAX) break;
        if (bestTrack < 0) {
            Check(writer->WriteSample(videoStream, MakeSample(snap.video[v++], rebase).get()), "WriteSample(video)");
        } else {
            const auto& p = snap.audio[bestTrack][next[bestTrack]++];
            Check(writer->WriteSample(audioStreams[bestTrack], MakeSample(p, rebase).get()), "WriteSample(audio)");
        }
    }
    Check(writer->Finalize(), "Finalize");
    writer = nullptr;

    ClipSaveResult r;
    if (!snap.video.empty())
        r.seconds = (snap.video.back().pts + snap.video.back().duration - rebase) / 1e7;
    r.namesWritten = Mp4SetTrackNames(in.path, "Video", in.audioTrackNames);
    std::error_code ec;
    r.bytes = std::filesystem::file_size(in.path, ec);
    return r;
}

}  // namespace wl
