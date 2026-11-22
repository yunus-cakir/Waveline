// Waveline Clip - M0 technical prototype
//
// Goal: prove the low-overhead pipeline and measure its cost before building the real
// replay buffer (M1):
//   Windows.Graphics.Capture -> D3D11 video processor (BGRA->NV12, GPU) -> NVENC (HEVC/H.264)
//   + 4 WASAPI sources (Game/Chat/Media via loopback, Mic) + generated Mix track
//   -> MP4 with 5 AAC tracks, plus a resource usage report.

#include "audio.h"
#include "capture.h"
#include "metrics.h"
#include "videoproc.h"
#include "writer.h"

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <atomic>
#include <crtdbg.h>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace wl;

namespace {

std::atomic<bool> g_stop{false};

// Diagnostic: prints the current call stack with symbols (needs the .pdb next to the exe).
void PrintStack() {
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    void* frames[48];
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    for (USHORT i = 0; i < count; ++i) {
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        const bool hasSym = SymFromAddr(process, addr, &displacement, symbol) != FALSE;
        const bool hasLine = SymGetLineFromAddr64(process, addr, &lineDisp, &line) != FALSE;
        std::fprintf(stderr, "  #%02u %s", i, hasSym ? symbol->Name : "?");
        if (hasLine) std::fprintf(stderr, "  (%s:%lu)", line.FileName, line.LineNumber);
        std::fprintf(stderr, "\n");
    }
}

BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

struct Options {
    bool list = false;
    int monitor = -1;  // -1 = primary
    bool window = false;
    int windowDelaySec = 3;
    int seconds = 30;
    UINT fps = 60;
    UINT kbps = 15000;
    bool hevc = true;
    bool cursor = true;
    std::wstring out;
};

void PrintUsage() {
    std::printf(
        "Waveline Clip - M0 prototip\n\n"
        "Kullanim:\n"
        "  WavelineClipProto --list\n"
        "      Monitorleri, ses cihazlarini (Sonar rolleriyle) ve donanim encoder'larini listeler.\n"
        "  WavelineClipProto [secenekler]\n"
        "      --monitor N       N numarali monitoru kaydet (varsayilan: ana monitor)\n"
        "      --window [sn]     sn saniye sonra one gelen pencereyi kaydet (varsayilan 3)\n"
        "      --seconds N       kayit suresi (varsayilan 30, Ctrl+C ile erken bitir)\n"
        "      --fps N           varsayilan 60\n"
        "      --bitrate KBPS    varsayilan 15000\n"
        "      --codec hevc|h264 varsayilan hevc\n"
        "      --no-cursor       imleci kaydetme\n"
        "      --out DOSYA.mp4   cikti dosyasi\n");
}

Options ParseArgs(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) throw std::runtime_error("Eksik arguman: " + ToUtf8(a));
            return argv[++i];
        };
        if (a == L"--list") o.list = true;
        else if (a == L"--monitor") o.monitor = std::stoi(next());
        else if (a == L"--window") {
            o.window = true;
            if (i + 1 < argc && iswdigit(argv[i + 1][0])) o.windowDelaySec = std::stoi(argv[++i]);
        }
        else if (a == L"--seconds") o.seconds = std::stoi(next());
        else if (a == L"--fps") o.fps = static_cast<UINT>(std::stoi(next()));
        else if (a == L"--bitrate") o.kbps = static_cast<UINT>(std::stoi(next()));
        else if (a == L"--codec") o.hevc = next() != L"h264";
        else if (a == L"--no-cursor") o.cursor = false;
        else if (a == L"--out") o.out = next();
        else if (a == L"--help" || a == L"-h" || a == L"/?") { PrintUsage(); std::exit(0); }
        else throw std::runtime_error("Bilinmeyen arguman: " + ToUtf8(a));
    }
    if (o.fps < 1 || o.fps > 240) throw std::runtime_error("--fps 1-240 arasinda olmali");
    if (o.seconds < 1) throw std::runtime_error("--seconds en az 1 olmali");
    return o;
}

struct MonitorEntry {
    HMONITOR handle;
    std::wstring device;
    RECT rect;
    bool primary;
};

std::vector<MonitorEntry> EnumerateMonitors() {
    std::vector<MonitorEntry> list;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(m, &mi);
            reinterpret_cast<std::vector<MonitorEntry>*>(p)->push_back(
                {m, mi.szDevice, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));
    return list;
}

const char* SonarRoleName(int role) {
    switch (role) {
        case kSonarChat: return "Chat";
        case kSonarGame: return "Game";
        case kSonarMic: return "Mic";
        case kSonarStream: return "Stream";
        case kSonarMedia: return "Media";
        case kSonarAux: return "Aux";
        default: return "-";
    }
}

void ListEncoders(const GUID& subtype, const char* label) {
    MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                         nullptr, &outType, &activates, &count))) {
        std::printf("  %s: sorgulanamadi\n", label);
        return;
    }
    if (count == 0) std::printf("  %s: donanim encoder'i YOK\n", label);
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
            std::printf("  %s: %s\n", label, ToUtf8(name).c_str());
            CoTaskMemFree(name);
        }
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
}

void RunList() {
    std::printf("== Monitorler\n");
    auto monitors = EnumerateMonitors();
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        std::printf("  [%zu] %s  %ldx%ld @ (%ld,%ld)%s\n", i, ToUtf8(m.device).c_str(),
                    m.rect.right - m.rect.left, m.rect.bottom - m.rect.top, m.rect.left, m.rect.top,
                    m.primary ? "  (ana)" : "");
    }
    std::printf("\n== Ses cihazlari (aktif)\n");
    for (const auto& e : EnumerateEndpoints())
        std::printf("  %-7s  Sonar rolu: %-6s  %s\n", e.capture ? "Kayit" : "Cikis",
                    SonarRoleName(e.sonarRole), ToUtf8(e.name).c_str());
    std::printf("\n== Donanim video encoder'lari\n");
    ListEncoders(MFVideoFormat_HEVC, "HEVC");
    ListEncoders(MFVideoFormat_H264, "H.264");
}

// Picks the device for each track. With Sonar installed: its Gaming/Chat/Media/Microphone
// endpoints. Without Sonar: Game = default output (all system audio), Mic = default mic.
void SetupAudio(AudioEngine& engine) {
    const auto endpoints = EnumerateEndpoints();
    auto find = [&](int role, bool capture, const wchar_t* nameHint) -> const EndpointInfo* {
        for (const auto& e : endpoints)
            if (e.capture == capture && e.sonarRole == role) return &e;
        for (const auto& e : endpoints)
            if (e.capture == capture && e.name.find(nameHint) != std::wstring::npos) return &e;
        return nullptr;
    };

    struct Want { Track track; int role; bool capture; const wchar_t* hint; };
    const Want wants[] = {
        {Track::Game, kSonarGame, false, L"Sonar - Gaming"},
        {Track::Chat, kSonarChat, false, L"Sonar - Chat"},
        {Track::Media, kSonarMedia, false, L"Sonar - Media"},
        {Track::Mic, kSonarMic, true, L"Sonar - Microphone"},
    };

    const bool sonarPresent = find(kSonarGame, false, L"Sonar - Gaming") != nullptr;
    if (!sonarPresent)
        Warn("Sonar bulunamadi: Game izi = varsayilan hoparlorun tum sesi, Chat/Media sessiz, Mic = varsayilan mikrofon");

    for (const auto& w : wants) {
        std::wstring id, name;
        if (const EndpointInfo* e = find(w.role, w.capture, w.hint)) {
            id = e->id;
            name = e->name;
        } else if (!sonarPresent && (w.track == Track::Game || w.track == Track::Mic)) {
            id = DefaultEndpointId(w.capture);
            for (const auto& candidate : endpoints)
                if (candidate.id == id) name = candidate.name;
        }

        if (id.empty()) {
            Warn("%s izi icin cihaz yok, iz sessiz yazilacak", TrackName(w.track));
            continue;
        }
        auto source = std::make_unique<AudioSource>(id, TrackName(w.track), !w.capture);
        try {
            source->Open();
            Log("Ses: %-5s <- %s%s", TrackName(w.track), ToUtf8(name).c_str(), w.capture ? "" : " (loopback)");
            engine.SetSource(w.track, std::move(source));
        } catch (const HrError& e) {
            Warn("%s izi acilamadi (%s, 0x%08X), iz sessiz yazilacak", TrackName(w.track), e.what(),
                 static_cast<unsigned>(e.hr));
        }
    }
}

std::wstring DefaultOutputPath() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"waveline_m0_%04u%02u%02u_%02u%02u%02u.mp4", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return buf;
}

com_ptr<IMFMediaType> MakeNv12Type(UINT w, UINT h, UINT fps) {
    com_ptr<IMFMediaType> t;
    Check(MFCreateMediaType(t.put()), "MFCreateMediaType");
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(t.get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t.get(), MF_MT_FRAME_RATE, fps, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return t;
}

int RunRecord(const Options& opt) {
    // --- capture target ---
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    std::string targetName;
    if (opt.window) {
        Log("%d saniye icinde kaydedilecek pencereye gecin...", opt.windowDelaySec);
        Sleep(static_cast<DWORD>(opt.windowDelaySec) * 1000);
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) throw std::runtime_error("On planda pencere yok");
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 256);
        targetName = "pencere: " + ToUtf8(title);
        item = CaptureSource::ItemForWindow(hwnd);
    } else {
        auto monitors = EnumerateMonitors();
        const MonitorEntry* m = nullptr;
        if (opt.monitor >= 0) {
            if (static_cast<size_t>(opt.monitor) >= monitors.size()) throw std::runtime_error("Gecersiz monitor numarasi");
            m = &monitors[static_cast<size_t>(opt.monitor)];
        } else {
            for (const auto& e : monitors)
                if (e.primary) m = &e;
            if (!m && !monitors.empty()) m = &monitors[0];
        }
        if (!m) throw std::runtime_error("Monitor bulunamadi");
        targetName = "monitor: " + ToUtf8(m->device);
        item = CaptureSource::ItemForMonitor(m->handle);
    }

    // --- GPU + capture ---
    GpuDevice gpu = CreateGpuDevice();
    Log("GPU: %s (vendor 0x%04X)", gpu.adapterName.c_str(), gpu.vendorId);
    if (gpu.vendorId != 0x10DE) Warn("NVIDIA GPU secilmedi, NVENC kullanilamayabilir");

    UINT resetToken = 0;
    com_ptr<IMFDXGIDeviceManager> deviceManager;
    Check(MFCreateDXGIDeviceManager(&resetToken, deviceManager.put()), "MFCreateDXGIDeviceManager");
    Check(deviceManager->ResetDevice(gpu.device.get(), resetToken), "IMFDXGIDeviceManager::ResetDevice");

    CaptureSource capture(item);
    capture.Start(gpu, opt.cursor);
    Log("Yakalama basladi (%s), sari cerceve %s", targetName.c_str(), capture.BorderDisabled() ? "kapali" : "acik");

    VideoConverter converter;
    CapturedFrame first;
    const int64_t waitUntil = QpcNow100ns() + 3 * kHns;
    while (!capture.TakeLatest(first)) {
        if (QpcNow100ns() > waitUntil) throw std::runtime_error("3 saniyede hic kare gelmedi");
        Sleep(5);
    }
    const UINT outW = first.contentWidth & ~1u;
    const UINT outH = first.contentHeight & ~1u;
    converter.Init(gpu, outW, outH, opt.fps);
    converter.Upload(first.texture.get(), first.contentWidth, first.contentHeight);
    first.frame.Close();
    first = {};

    // --- writer ---
    WriterConfig wc;
    wc.path = opt.out.empty() ? DefaultOutputPath() : opt.out;
    wc.path = std::filesystem::absolute(wc.path).wstring();
    wc.width = outW;
    wc.height = outH;
    wc.fps = opt.fps;
    wc.videoKbps = opt.kbps;
    wc.hevc = opt.hevc;
    wc.audioTracks = kTrackCount;
    ClipWriter writer;
    writer.Open(wc, deviceManager.get());
    Log("Encoder: %s (%s), %ux%u @ %u FPS, %s %u kbps", writer.VideoEncoderName().c_str(),
        writer.VideoEncoderIsHardware() ? "donanim" : "YAZILIM!", outW, outH, opt.fps, opt.hevc ? "HEVC" : "H.264",
        opt.kbps);
    if (!writer.VideoEncoderIsHardware()) Warn("Donanim encoder'i kullanilmiyor, CPU yuku yuksek olacak");

    // --- NV12 sample pool (GPU textures, recycled when the encoder releases them) ---
    com_ptr<IMFVideoSampleAllocatorEx> allocator;
    Check(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(allocator.put())), "MFCreateVideoSampleAllocatorEx");
    Check(allocator->SetDirectXManager(deviceManager.get()), "SetDirectXManager");
    com_ptr<IMFAttributes> allocAttrs;
    Check(MFCreateAttributes(allocAttrs.put(), 2), "MFCreateAttributes");
    allocAttrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER);
    allocAttrs->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
    auto nv12 = MakeNv12Type(outW, outH, opt.fps);
    Check(allocator->InitializeSampleAllocatorEx(4, 16, allocAttrs.get(), nv12.get()), "InitializeSampleAllocatorEx");

    // --- audio ---
    AudioEngine audio;
    SetupAudio(audio);

    Metrics metrics;

    // --- record loop: constant frame rate, newest captured frame each tick ---
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    const int64_t frameHns = kHns / opt.fps;
    const uint64_t totalFrames = static_cast<uint64_t>(opt.seconds) * opt.fps;
    uint64_t encoded = 0, fresh = 0, repeated = 0, poolEmpty = 0, late = 0;

    const int64_t startHns = QpcNow100ns();
    audio.Start(&writer, startHns);
    const uint64_t arrivedAtStart = capture.ArrivedFrames();
    Log("Kayit: %d sn (Ctrl+C ile erken bitir) -> %s", opt.seconds, ToUtf8(wc.path).c_str());

    uint64_t frameIndex = 0;
    for (; frameIndex < totalFrames && !g_stop && !capture.Closed(); ++frameIndex) {
        const int64_t due = startHns + static_cast<int64_t>(frameIndex) * kHns / opt.fps;
        const int64_t now = QpcNow100ns();
        if (due > now) {
            LARGE_INTEGER li;
            li.QuadPart = -(due - now);
            SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else if (now - due > 2 * frameHns) {
            ++late;
        }

        CapturedFrame f;
        if (capture.TakeLatest(f)) {
            converter.Upload(f.texture.get(), f.contentWidth, f.contentHeight);
            f.frame.Close();
            ++fresh;
        } else {
            ++repeated;  // nothing new on screen: repeat the previous frame
        }

        com_ptr<IMFSample> sample;
        HRESULT hr = allocator->AllocateSample(sample.put());
        if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) {
            ++poolEmpty;  // encoder is behind; skip this tick
            continue;
        }
        Check(hr, "AllocateSample");
        com_ptr<IMFMediaBuffer> buffer;
        Check(sample->GetBufferByIndex(0, buffer.put()), "GetBufferByIndex");
        auto dxgiBuffer = buffer.as<IMFDXGIBuffer>();
        com_ptr<ID3D11Texture2D> texture;
        Check(dxgiBuffer->GetResource(IID_PPV_ARGS(texture.put())), "IMFDXGIBuffer::GetResource");
        UINT subresource = 0;
        dxgiBuffer->GetSubresourceIndex(&subresource);
        // Pooled GPU buffers come back with length 0; the encoder rejects empty buffers.
        if (auto buffer2d = buffer.try_as<IMF2DBuffer>()) {
            DWORD length = 0;
            if (SUCCEEDED(buffer2d->GetContiguousLength(&length))) buffer->SetCurrentLength(length);
        }

        converter.Convert(texture.get(), subresource);
        writer.WriteVideo(sample.get(), static_cast<int64_t>(frameIndex) * kHns / opt.fps, frameHns);
        ++encoded;

        if ((frameIndex + 1) % opt.fps == 0) {
            metrics.Tick();
            const auto& m = metrics.Last();
            std::printf("\r  %3llus | kare %llu (yeni %llu, tekrar %llu) | CPU %%%.1f | NVENC %%%.0f (sistem %%%.0f) | 3D %%%.0f | RAM %.0f MB | VRAM %.0f MB   ",
                        static_cast<unsigned long long>((frameIndex + 1) / opt.fps),
                        static_cast<unsigned long long>(encoded), static_cast<unsigned long long>(fresh),
                        static_cast<unsigned long long>(repeated), m.cpuPercent, m.nvencPercent,
                        m.nvencSystemPercent, m.gpu3dPercent, m.workingSetMB, m.gpuDedicatedMB);
            std::fflush(stdout);
        }
    }
    std::printf("\n");
    if (capture.Closed()) Warn("Kaydedilen pencere kapandi");

    const uint64_t arrived = capture.ArrivedFrames() - arrivedAtStart;
    const int64_t elapsedHns = QpcNow100ns() - startHns;
    const int64_t endAudioFrame = static_cast<int64_t>(frameIndex) * kSampleRate / opt.fps;

    capture.Stop();
    audio.StopAt(endAudioFrame);
    writer.Finalize();
    CloseHandle(timer);

    // --- report ---
    const auto avg = metrics.Average();
    const auto peak = metrics.Peak();
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(wc.path, ec);
    const double seconds = static_cast<double>(frameIndex) / opt.fps;

    std::ostringstream r;
    r.setf(std::ios::fixed);
    r.precision(1);
    r << "=== Waveline Clip M0 raporu ===\n"
      << "Dosya        : " << ToUtf8(wc.path) << "\n"
      << "Boyut        : " << (bytes / (1024.0 * 1024.0)) << " MB (" << (seconds > 0 ? bytes * 8.0 / seconds / 1e6 : 0)
      << " Mbps ortalama)\n"
      << "Hedef        : " << targetName << "\n"
      << "GPU          : " << gpu.adapterName << "\n"
      << "Encoder      : " << writer.VideoEncoderName() << (writer.VideoEncoderIsHardware() ? " (donanim)" : " (YAZILIM)")
      << ", " << (opt.hevc ? "HEVC" : "H.264") << " " << opt.kbps << " kbps, " << outW << "x" << outH << " @ " << opt.fps << "\n"
      << "Sure         : " << seconds << " sn video, " << (elapsedHns / 1e7) << " sn gercek\n"
      << "Kareler      : kodlanan " << encoded << ", yeni " << fresh << ", tekrar " << repeated
      << ", havuz-dolu-atlanan " << poolEmpty << ", gec-kalan-tik " << late << "\n"
      << "WGC kareleri : " << arrived << " geldi (" << (elapsedHns > 0 ? arrived * 1e7 / elapsedHns : 0) << "/sn)\n"
      << "WriteSample  : ort " << writer.AvgVideoWriteMs() << " ms, en fazla " << writer.MaxVideoWriteMs() << " ms\n"
      << "CPU          : ort %" << avg.cpuPercent << ", tepe %" << peak.cpuPercent << " (tum cekirdeklerin)\n"
      << "NVENC        : bu surec ort %" << avg.nvencPercent << ", tepe %" << peak.nvencPercent
      << " | sistem toplami ort %" << avg.nvencSystemPercent << "\n"
      << "GPU 3D       : bu surec ort %" << avg.gpu3dPercent << ", tepe %" << peak.gpu3dPercent << "\n"
      << "RAM          : ort " << avg.workingSetMB << " MB, tepe " << peak.workingSetMB << " MB (ozel tepe "
      << peak.privateMB << " MB)\n"
      << "VRAM         : ort " << avg.gpuDedicatedMB << " MB, tepe " << peak.gpuDedicatedMB << " MB\n"
      << "Mix kirpilan ornek: " << audio.MixClippedSamples() << "\n"
      << "Ses izleri (ms):\n";
    for (int t = static_cast<int>(Track::Game); t < kTrackCount; ++t) {
        AudioSource* s = audio.Source(static_cast<Track>(t));
        r << "  " << TrackName(static_cast<Track>(t)) << ": ";
        if (!s) {
            r << "cihaz yok (sessiz iz)\n";
            continue;
        }
        const auto st = s->Stats();
        auto ms = [](uint64_t frames) { return frames * 1000.0 / kSampleRate; };
        r << "paket " << st.packets << ", alinan " << ms(st.frames) << ", bosluk-dolgu " << ms(st.gapFilledFrames)
          << ", sessizlik-dolgu " << ms(st.deadlineFilledFrames) << ", atilan-cakisma " << ms(st.overlapDroppedFrames)
          << ", kesinti " << st.discontinuities << (st.deviceLost ? ", CIHAZ KAYBOLDU" : "") << "\n";
    }

    const std::string report = r.str();
    std::printf("\n%s", report.c_str());
    std::ofstream(wc.path + L".m0.txt", std::ios::binary) << report;
    Log("Rapor kaydedildi: %s.m0.txt", ToUtf8(wc.path).c_str());
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::set_terminate([] {
        // Any exception escaping a worker thread ends up here: print it before aborting.
        try {
            if (auto ex = std::current_exception()) std::rethrow_exception(ex);
            std::fprintf(stderr, "\nKRITIK: std::terminate (istisna yok)\n");
        } catch (const HrError& e) {
            std::fprintf(stderr, "\nKRITIK: %s (HRESULT 0x%08X)\n", e.what(), static_cast<unsigned>(e.hr));
        } catch (const winrt::hresult_error& e) {
            std::fprintf(stderr, "\nKRITIK: %s (HRESULT 0x%08X)\n", ToUtf8(e.message().c_str()).c_str(),
                         static_cast<unsigned>(e.code().value));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "\nKRITIK: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "\nKRITIK: bilinmeyen istisna\n");
        }
        PrintStack();
        std::fflush(stderr);
        std::abort();
    });
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    try {
        const Options opt = ParseArgs(argc, argv);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Check(MFStartup(MF_VERSION), "MFStartup");
        int rc = opt.list ? (RunList(), 0) : RunRecord(opt);
        MFShutdown();
        return rc;
    } catch (const HrError& e) {
        std::fprintf(stderr, "\nHATA: %s (HRESULT 0x%08X)\n", e.what(), static_cast<unsigned>(e.hr));
    } catch (const winrt::hresult_error& e) {
        std::fprintf(stderr, "\nHATA: %s (HRESULT 0x%08X)\n", ToUtf8(e.message().c_str()).c_str(),
                     static_cast<unsigned>(e.code().value));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nHATA: %s\n", e.what());
    }
    return 1;
}
