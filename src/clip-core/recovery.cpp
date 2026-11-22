#include "recovery.h"
#include "clip_saver.h"

#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace wl {

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTypesFile[] = L"\\types.bin";
constexpr wchar_t kNamesFile[] = L"\\names.txt";

uint32_t U32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t U64(const uint8_t* p) { return (uint64_t(U32(p)) << 32) | U32(p + 4); }

// ---- minimal MP4 sample-table reader ---------------------------------------------------

struct Sample {
    uint64_t offset = 0;
    uint32_t size = 0;
    int64_t time = 0;      // 100 ns, from the start of the segment
    int64_t duration = 0;  // 100 ns
    bool key = true;
};

struct Track {
    std::string handler;  // "vide" / "soun"
    std::vector<Sample> samples;
    int64_t durationHns = 0;
};

struct Boxes {
    std::map<std::string, std::vector<uint8_t>> leaf;  // payloads of the boxes we need
};

void Walk(const uint8_t* p, size_t n, Boxes& out) {
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t size = U32(p + i);
        const std::string type(reinterpret_cast<const char*>(p + i + 4), 4);
        size_t header = 8;
        if (size == 1 && i + 16 <= n) {
            size = U64(p + i + 8);
            header = 16;
        } else if (size == 0) {
            size = n - i;
        }
        if (size < header || i + size > n) return;
        const uint8_t* body = p + i + header;
        const size_t bodyLen = static_cast<size_t>(size) - header;
        if (type == "mdia" || type == "minf" || type == "stbl")
            Walk(body, bodyLen, out);
        else if (type == "mdhd" || type == "hdlr" || type == "stts" || type == "stsz" || type == "stsc" ||
                 type == "stco" || type == "co64" || type == "stss")
            out.leaf[type].assign(body, body + bodyLen);
        i += static_cast<size_t>(size);
    }
}

bool BuildTrack(const Boxes& b, Track& t) {
    auto get = [&](const char* k) -> const std::vector<uint8_t>* {
        auto it = b.leaf.find(k);
        return it == b.leaf.end() ? nullptr : &it->second;
    };
    const auto* mdhd = get("mdhd");
    const auto* hdlr = get("hdlr");
    const auto* stts = get("stts");
    const auto* stsz = get("stsz");
    const auto* stsc = get("stsc");
    const auto* stco = get("stco");
    const auto* co64 = get("co64");
    if (!mdhd || !hdlr || !stts || !stsz || !stsc || (!stco && !co64) || hdlr->size() < 12) return false;

    t.handler.assign(reinterpret_cast<const char*>(hdlr->data() + 8), 4);
    const bool v1 = (*mdhd)[0] == 1;
    if (mdhd->size() < (v1 ? 24u : 16u)) return false;
    const uint32_t timescale = U32(mdhd->data() + (v1 ? 20 : 12));
    if (!timescale) return false;

    // sizes
    const uint32_t fixed = U32(stsz->data() + 4), count = U32(stsz->data() + 8);
    if (!fixed && stsz->size() < 12 + size_t(count) * 4) return false;
    t.samples.resize(count);
    for (uint32_t i = 0; i < count; ++i) t.samples[i].size = fixed ? fixed : U32(stsz->data() + 12 + i * 4);

    // durations
    uint64_t ticks = 0;
    size_t s = 0;
    const uint32_t sttsCount = U32(stts->data() + 4);
    for (uint32_t e = 0; e < sttsCount && 8 + e * 8 + 8 <= stts->size(); ++e) {
        const uint32_t n = U32(stts->data() + 8 + e * 8), delta = U32(stts->data() + 12 + e * 8);
        for (uint32_t k = 0; k < n && s < count; ++k, ++s) {
            t.samples[s].time = static_cast<int64_t>(ticks * kHns / timescale);
            ticks += delta;
            t.samples[s].duration = static_cast<int64_t>(ticks * kHns / timescale) - t.samples[s].time;
        }
    }
    t.durationHns = static_cast<int64_t>(ticks * kHns / timescale);

    // chunk offsets
    std::vector<uint64_t> chunks;
    if (co64) {
        const uint32_t n = U32(co64->data() + 4);
        for (uint32_t i = 0; i < n && 8 + i * 8 + 8 <= co64->size(); ++i) chunks.push_back(U64(co64->data() + 8 + i * 8));
    } else {
        const uint32_t n = U32(stco->data() + 4);
        for (uint32_t i = 0; i < n && 8 + i * 4 + 4 <= stco->size(); ++i) chunks.push_back(U32(stco->data() + 8 + i * 4));
    }

    // samples -> chunks
    const uint32_t stscCount = U32(stsc->data() + 4);
    size_t sample = 0;
    for (uint32_t e = 0; e < stscCount; ++e) {
        const uint32_t first = U32(stsc->data() + 8 + e * 12);
        const uint32_t perChunk = U32(stsc->data() + 12 + e * 12);
        const uint32_t last = e + 1 < stscCount ? U32(stsc->data() + 8 + (e + 1) * 12) - 1 : static_cast<uint32_t>(chunks.size());
        for (uint32_t c = first; c <= last && c >= 1 && c <= chunks.size(); ++c) {
            uint64_t off = chunks[c - 1];
            for (uint32_t k = 0; k < perChunk && sample < count; ++k, ++sample) {
                t.samples[sample].offset = off;
                off += t.samples[sample].size;
            }
        }
    }
    if (sample != count) return false;

    // sync samples (absent = every sample is a sync sample, e.g. audio)
    if (const auto* stss = get("stss")) {
        for (auto& smp : t.samples) smp.key = false;
        const uint32_t n = U32(stss->data() + 4);
        for (uint32_t i = 0; i < n && 8 + i * 4 + 4 <= stss->size(); ++i) {
            const uint32_t num = U32(stss->data() + 8 + i * 4);
            if (num >= 1 && num <= count) t.samples[num - 1].key = true;
        }
    }
    return true;
}

// Returns false when the file has no readable 'moov' (e.g. still being written at a crash).
bool ParseMp4(const std::wstring& path, std::vector<Track>& tracks) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(f.tellg());
    uint64_t pos = 0;
    while (pos + 8 <= fileSize) {
        uint8_t h[16];
        f.seekg(static_cast<std::streamoff>(pos));
        f.read(reinterpret_cast<char*>(h), 8);
        uint64_t size = U32(h);
        const std::string type(reinterpret_cast<char*>(h + 4), 4);
        uint64_t header = 8;
        if (size == 1) {
            f.read(reinterpret_cast<char*>(h + 8), 8);
            size = U64(h + 8);
            header = 16;
        } else if (size == 0) {
            size = fileSize - pos;
        }
        if (size < header || pos + size > fileSize) return false;
        if (type == "moov") {
            std::vector<uint8_t> moov(static_cast<size_t>(size - header));
            f.read(reinterpret_cast<char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
            size_t i = 0;
            while (i + 8 <= moov.size()) {
                const uint32_t bs = U32(moov.data() + i);
                const std::string bt(reinterpret_cast<const char*>(moov.data() + i + 4), 4);
                if (bs < 8 || i + bs > moov.size()) break;
                if (bt == "trak") {
                    Boxes boxes;
                    Walk(moov.data() + i + 8, bs - 8, boxes);
                    Track t;
                    if (!BuildTrack(boxes, t)) return false;
                    tracks.push_back(std::move(t));
                }
                i += bs;
            }
            return !tracks.empty();
        }
        pos += size;
    }
    return false;
}

// MP4 stores H.264/HEVC with 4-byte NAL lengths; the encoder path (and the sink writer's
// pass-through input) uses Annex-B start codes.
void LengthPrefixedToAnnexB(std::vector<uint8_t>& d) {
    size_t i = 0;
    while (i + 4 <= d.size()) {
        const uint32_t len = U32(d.data() + i);
        d[i] = 0; d[i + 1] = 0; d[i + 2] = 0; d[i + 3] = 1;
        i += 4 + len;
    }
}

com_ptr<IMFSample> MakeSample(const std::vector<uint8_t>& data, int64_t time, int64_t duration, bool key) {
    com_ptr<IMFMediaBuffer> buffer;
    Check(MFCreateMemoryBuffer(static_cast<DWORD>(data.size()), buffer.put()), "MFCreateMemoryBuffer");
    BYTE* dst = nullptr;
    Check(buffer->Lock(&dst, nullptr, nullptr), "Lock");
    std::memcpy(dst, data.data(), data.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(data.size()));
    com_ptr<IMFSample> s;
    Check(MFCreateSample(s.put()), "MFCreateSample");
    s->AddBuffer(buffer.get());
    s->SetSampleTime(time);
    s->SetSampleDuration(duration);
    if (key) s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    return s;
}

bool ReadRecoveryInfo(const std::wstring& dir, std::vector<com_ptr<IMFMediaType>>& types, std::vector<std::string>& names) {
    std::ifstream f(dir + kTypesFile, std::ios::binary);
    if (!f) return false;
    uint32_t n = 0;
    f.read(reinterpret_cast<char*>(&n), 4);
    for (uint32_t i = 0; i < n && f; ++i) {
        uint32_t len = 0;
        f.read(reinterpret_cast<char*>(&len), 4);
        std::vector<uint8_t> blob(len);
        f.read(reinterpret_cast<char*>(blob.data()), len);
        com_ptr<IMFMediaType> t;
        Check(MFCreateMediaType(t.put()), "MFCreateMediaType");
        Check(MFInitAttributesFromBlob(t.get(), blob.data(), len), "MFInitAttributesFromBlob");
        types.push_back(t);
    }
    std::ifstream nf(dir + kNamesFile, std::ios::binary);
    std::string line;
    while (std::getline(nf, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) names.push_back(line);
    }
    return types.size() >= 2;
}

// Rebuilds one recording; returns its length in seconds (0 if nothing was recoverable).
double RecoverOne(const std::wstring& partsDir, const std::wstring& finalPath) {
    std::vector<com_ptr<IMFMediaType>> types;
    std::vector<std::string> names;
    if (!ReadRecoveryInfo(partsDir, types, names)) {
        Warn("Kurtarma bilgisi yok, atlaniyor: %s", ToUtf8(partsDir).c_str());
        return 0;
    }
    std::vector<std::wstring> segments;
    for (const auto& e : fs::directory_iterator(partsDir))
        if (e.path().extension() == L".mp4") segments.push_back(e.path().wstring());
    std::sort(segments.begin(), segments.end());

    const std::wstring tmp = finalPath + L".recovering.mp4";
    com_ptr<IMFAttributes> attrs;
    Check(MFCreateAttributes(attrs.put(), 1), "MFCreateAttributes");
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    com_ptr<IMFSinkWriter> writer;
    Check(MFCreateSinkWriterFromURL(tmp.c_str(), nullptr, attrs.get(), writer.put()), "MFCreateSinkWriterFromURL(recover)");
    std::vector<DWORD> streams;
    for (const auto& t : types) {
        DWORD s = 0;
        Check(writer->AddStream(t.get(), &s), "AddStream(recover)");
        Check(writer->SetInputMediaType(s, t.get(), nullptr), "SetInputMediaType(recover)");
        streams.push_back(s);
    }
    Check(writer->BeginWriting(), "BeginWriting(recover)");

    int64_t base = 0;  // start of the current segment on the output timeline
    size_t used = 0;
    for (const auto& seg : segments) {
        std::vector<Track> tracks;
        if (!ParseMp4(seg, tracks) || tracks.size() != types.size() || tracks[0].handler != "vide") continue;
        std::ifstream data(seg, std::ios::binary);

        // interleave the segment's samples by time, video first on ties
        struct Item { int64_t time; size_t track; size_t index; };
        std::vector<Item> order;
        for (size_t t = 0; t < tracks.size(); ++t)
            for (size_t i = 0; i < tracks[t].samples.size(); ++i) order.push_back({tracks[t].samples[i].time, t, i});
        std::stable_sort(order.begin(), order.end(), [](const Item& a, const Item& b) { return a.time < b.time; });

        std::vector<uint8_t> buf;
        for (const auto& it : order) {
            const Sample& s = tracks[it.track].samples[it.index];
            buf.resize(s.size);
            data.seekg(static_cast<std::streamoff>(s.offset));
            data.read(reinterpret_cast<char*>(buf.data()), s.size);
            if (!data) break;
            if (it.track == 0) LengthPrefixedToAnnexB(buf);
            Check(writer->WriteSample(streams[it.track], MakeSample(buf, base + s.time, s.duration, s.key).get()),
                  "WriteSample(recover)");
        }
        base += tracks[0].durationHns;
        ++used;
    }
    if (used == 0) {
        writer = nullptr;
        std::error_code ec;
        fs::remove(tmp, ec);
        return 0;
    }
    Check(writer->Finalize(), "Finalize(recover)");
    writer = nullptr;

    std::error_code ec;
    fs::remove(finalPath, ec);  // the interrupted file has no index and cannot be played
    fs::rename(tmp, finalPath, ec);
    if (ec) return 0;
    Mp4SetTrackNames(finalPath, "Video", names);
    return base / 1e7;
}

bool PlayableMp4(const std::wstring& path) {
    std::vector<Track> t;
    return fs::exists(path) && ParseMp4(path, t);
}

}  // namespace

void WriteRecoveryInfo(const std::wstring& partsDir, IMFMediaType* video, const std::vector<com_ptr<IMFMediaType>>& audio,
                       const std::vector<std::string>& names) {
    std::ofstream f(partsDir + kTypesFile, std::ios::binary);
    std::vector<IMFMediaType*> all{video};
    for (const auto& a : audio) all.push_back(a.get());
    const uint32_t n = static_cast<uint32_t>(all.size());
    f.write(reinterpret_cast<const char*>(&n), 4);
    for (IMFMediaType* t : all) {
        UINT32 len = 0;
        MFGetAttributesAsBlobSize(t, &len);
        std::vector<uint8_t> blob(len);
        MFGetAttributesAsBlob(t, blob.data(), len);
        f.write(reinterpret_cast<const char*>(&len), 4);
        f.write(reinterpret_cast<const char*>(blob.data()), len);
    }
    std::ofstream nf(partsDir + kNamesFile, std::ios::binary);
    for (const auto& name : names) nf << name << "\n";
}

void RecoverInterruptedRecordings(const std::wstring& dir, const std::function<void(const std::wstring&, double)>& onRecovered) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> partsDirs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory() && e.path().extension() == L".parts") partsDirs.push_back(e.path());

    for (const auto& parts : partsDirs) {
        fs::path final = parts;
        final.replace_extension(L".mp4");
        if (PlayableMp4(final.wstring())) {  // finished fine, only the backup cleanup was missed
            fs::remove_all(parts, ec);
            continue;
        }
        Log("Yarim kalan kayit bulundu, kurtariliyor: %s", ToUtf8(final.wstring()).c_str());
        try {
            const double seconds = RecoverOne(parts.wstring(), final.wstring());
            if (seconds > 0) {
                fs::remove_all(parts, ec);
                Log("Kayit kurtarildi: %s (%.1f sn)", ToUtf8(final.wstring()).c_str(), seconds);
                if (onRecovered) onRecovered(final.wstring(), seconds);
            } else {
                Warn("Kurtarilacak tamamlanmis bolum yok: %s", ToUtf8(parts.wstring()).c_str());
            }
        } catch (const HrError& e) {
            Warn("Kayit kurtarilamadi (%s, 0x%08X), bolumler korunuyor: %s", e.what(), static_cast<unsigned>(e.hr),
                 ToUtf8(parts.wstring()).c_str());
        }
    }
}

}  // namespace wl
