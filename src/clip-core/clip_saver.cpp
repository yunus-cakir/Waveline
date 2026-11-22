#include "clip_saver.h"

#include <mfreadwrite.h>

#include <algorithm>
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

void Append(std::vector<uint8_t>& v, const void* p, size_t n) {
    v.insert(v.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
}
std::vector<uint8_t> MakeBox(const char* type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> b;
    WriteU32(b, static_cast<uint32_t>(payload.size() + 8));
    Append(b, type, 4);
    Append(b, payload.data(), payload.size());
    return b;
}

std::vector<uint8_t> IlstItem(const char* fourcc, const std::string& utf8) {
    std::vector<uint8_t> data;
    WriteU32(data, 1);  // type: UTF-8 text
    WriteU32(data, 0);  // locale
    Append(data, utf8.data(), utf8.size());
    return MakeBox(fourcc, MakeBox("data", data));
}

// Microsoft 'Xtra' box: Windows Explorer reads "WM/Category" as the file's Tags.
std::vector<uint8_t> XtraBox(const std::wstring& category) {
    const std::string name = "WM/Category";
    std::vector<uint8_t> value;  // UTF-16LE, null-terminated
    for (wchar_t c : category) { value.push_back(uint8_t(c & 0xFF)); value.push_back(uint8_t(c >> 8)); }
    value.push_back(0);
    value.push_back(0);
    const uint32_t valueSize = static_cast<uint32_t>(4 + 2 + value.size());
    std::vector<uint8_t> entry;
    WriteU32(entry, static_cast<uint32_t>(4 + 4 + name.size() + 4 + valueSize));
    WriteU32(entry, static_cast<uint32_t>(name.size()));
    Append(entry, name.data(), name.size());
    WriteU32(entry, 1);  // one value
    WriteU32(entry, valueSize);
    entry.push_back(0);
    entry.push_back(8);  // type 8 = Unicode string
    Append(entry, value.data(), value.size());
    return MakeBox("Xtra", entry);
}

constexpr char kFreeformDomain[] = "com.waveline.clip";

// iTunes "freeform" item ('----' with mean/name/data): our own keys, kept by taggers and
// shown by ffprobe as plain tags.
std::vector<uint8_t> FreeformItem(const std::string& key, const std::string& utf8) {
    std::vector<uint8_t> mean(4, 0), name(4, 0), data;
    Append(mean, kFreeformDomain, sizeof(kFreeformDomain) - 1);
    Append(name, key.data(), key.size());
    WriteU32(data, 1);
    WriteU32(data, 0);
    Append(data, utf8.data(), utf8.size());
    std::vector<uint8_t> item;
    for (const auto& part : {MakeBox("mean", mean), MakeBox("name", name), MakeBox("data", data)})
        Append(item, part.data(), part.size());
    return MakeBox("----", item);
}

std::wstring NowIso() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t day[32];
    swprintf_s(day, L"%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return day;
}

std::vector<uint8_t> UdtaPayload(const ClipMetadata& m) {
    std::vector<uint8_t> ilst;
    auto add = [&](const char* fourcc, const std::wstring& v) {
        if (v.empty()) return;
        const auto item = IlstItem(fourcc, ToUtf8(v));
        Append(ilst, item.data(), item.size());
    };
    auto addFree = [&](const char* key, const std::string& v) {
        if (v.empty()) return;
        const auto item = FreeformItem(key, v);
        Append(ilst, item.data(), item.size());
    };
    add("\xA9nam", m.title);
    add("\xA9" "alb", m.game);
    add("\xA9gen", m.game);
    add("\xA9" "cmt", m.comment);
    add("\xA9too", L"Waveline Clip");
    add("\xA9" "day", m.date.empty() ? NowIso() : m.date);
    addFree("id", ToUtf8(m.id));
    addFree("kind", ToUtf8(m.kind));
    addFree("game", ToUtf8(m.game));
    addFree("edit", m.editJson);

    std::vector<uint8_t> hdlr(8, 0);  // version/flags, pre_defined
    Append(hdlr, "mdirappl", 8);      // handler type + reserved[0]
    hdlr.insert(hdlr.end(), 9, 0);    // reserved[1..2] + empty name
    std::vector<uint8_t> meta(4, 0);  // full box version/flags
    const auto h = MakeBox("hdlr", hdlr);
    const auto i = MakeBox("ilst", ilst);
    Append(meta, h.data(), h.size());
    Append(meta, i.data(), i.size());

    std::vector<uint8_t> udta = MakeBox("meta", meta);
    if (!m.game.empty()) {
        const auto x = XtraBox(m.game);
        Append(udta, x.data(), x.size());
    }
    return udta;
}

// Parses udta -> meta -> ilst back into ClipMetadata.
void ReadUdta(const std::vector<uint8_t>& udta, ClipMetadata& m) {
    auto children = [](const uint8_t* p, size_t n, auto&& fn) {
        size_t i = 0;
        while (i + 8 <= n) {
            const uint32_t size = ReadU32(p + i);
            if (size < 8 || i + size > n) return;
            fn(std::string(reinterpret_cast<const char*>(p + i + 4), 4), p + i + 8, size - 8);
            i += size;
        }
    };
    auto fromUtf8 = [](const uint8_t* p, size_t n) {
        const int len = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), static_cast<int>(n), nullptr, 0);
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), static_cast<int>(n), w.data(), len);
        return w;
    };
    children(udta.data(), udta.size(), [&](const std::string& t, const uint8_t* p, size_t n) {
        if (t != "meta" || n < 4) return;
        children(p + 4, n - 4, [&](const std::string& t2, const uint8_t* p2, size_t n2) {
            if (t2 != "ilst") return;
            children(p2, n2, [&](const std::string& item, const uint8_t* ip, size_t in) {
                std::string key;
                const uint8_t* value = nullptr;
                size_t valueLen = 0;
                children(ip, in, [&](const std::string& part, const uint8_t* pp, size_t pn) {
                    if (part == "name" && pn >= 4) key.assign(reinterpret_cast<const char*>(pp + 4), pn - 4);
                    if (part == "data" && pn >= 8) { value = pp + 8; valueLen = pn - 8; }
                });
                if (!value) return;
                const std::wstring w = fromUtf8(value, valueLen);
                if (item == "\xA9nam") m.title = w;
                else if (item == "\xA9" "alb") m.game = w;
                else if (item == "\xA9" "cmt") m.comment = w;
                else if (item == "\xA9" "day") m.date = w;
                else if (item == "----" && key == "id") m.id = w;
                else if (item == "----" && key == "kind") m.kind = w;
                else if (item == "----" && key == "game") m.game = w;
                else if (item == "----" && key == "edit") m.editJson.assign(reinterpret_cast<const char*>(value), valueLen);
            });
        });
    });
}

// Loads the 'moov' box of an MP4 whose moov is the last top-level box.
bool LoadMoov(std::fstream& f, const std::wstring& path, uint64_t& moovPos, std::vector<Box>& top) {
    f.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) {
        f.clear();
        f.open(path, std::ios::in | std::ios::binary);  // read-only file: reading still works
        if (!f) return false;
    }
    f.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(f.tellg());
    uint64_t pos = 0, moovSize = 0;
    moovPos = 0;
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
    return ParseBoxes(raw.data(), raw.size(), top) && top.size() == 1;
}

bool SaveMoov(std::fstream& f, const std::wstring& path, uint64_t moovPos, const Box& moov) {
    std::vector<uint8_t> rebuilt;
    Serialize(moov, rebuilt);
    f.clear();
    f.seekp(static_cast<std::streamoff>(moovPos));
    f.write(reinterpret_cast<const char*>(rebuilt.data()), static_cast<std::streamsize>(rebuilt.size()));
    if (!f) return false;
    f.close();
    std::error_code ec;
    std::filesystem::resize_file(path, moovPos + rebuilt.size(), ec);
    return !ec;
}

void ReplaceUdta(Box& moov, const ClipMetadata& metadata) {
    auto& kids = moov.children;
    kids.erase(std::remove_if(kids.begin(), kids.end(), [](const Box& b) { return b.type == "udta"; }), kids.end());
    Box udta;
    udta.type = "udta";
    udta.payload = UdtaPayload(metadata);
    kids.push_back(std::move(udta));
}

}  // namespace

std::string DefaultEditJson(const std::vector<std::string>& trackNames) {
    std::string json = "{\"version\":1,\"favorite\":false,\"trimStart\":null,\"trimEnd\":null,\"tracks\":[";
    for (size_t i = 0; i < trackNames.size(); ++i) {
        if (i) json += ",";
        json += "{\"name\":\"" + trackNames[i] + "\",\"volume\":1.0,\"muted\":false}";
    }
    return json + "]}";
}

std::wstring NewClipId() {
    GUID g{};
    CoCreateGuid(&g);
    wchar_t buf[40];
    StringFromGUID2(g, buf, 40);
    std::wstring s(buf + 1, wcslen(buf) - 2);  // strip braces
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

bool Mp4ReadMetadata(const std::wstring& path, ClipMetadata& out) {
    std::fstream f;
    uint64_t moovPos = 0;
    std::vector<Box> top;
    if (!LoadMoov(f, path, moovPos, top)) return false;
    for (const auto& b : top[0].children)
        if (b.type == "udta") ReadUdta(b.payload, out);
    return true;
}

bool Mp4WriteMetadata(const std::wstring& path, const ClipMetadata& metadata) {
    std::fstream f;
    uint64_t moovPos = 0;
    std::vector<Box> top;
    if (!LoadMoov(f, path, moovPos, top)) return false;
    ReplaceUdta(top[0], metadata);
    return SaveMoov(f, path, moovPos, top[0]);
}

bool Mp4SetTrackNames(const std::wstring& path, const std::string& videoName, const std::vector<std::string>& audioNames,
                      const ClipMetadata& metadata) {
    std::fstream f;
    uint64_t moovPos = 0;
    std::vector<Box> top;
    if (!LoadMoov(f, path, moovPos, top)) return false;

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

    if (!metadata.Empty()) ReplaceUdta(top[0], metadata);
    return SaveMoov(f, path, moovPos, top[0]);
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
    r.namesWritten = Mp4SetTrackNames(in.path, "Video", in.audioTrackNames, in.metadata);
    std::error_code ec;
    r.bytes = std::filesystem::file_size(in.path, ec);
    return r;
}

}  // namespace wl
