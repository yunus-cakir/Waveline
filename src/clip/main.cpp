// Waveline Clip - M2: automatic game detection + desktop mode (console)
//
// Idle until a game is detected (borderless/fullscreen foreground window, not a browser or
// media player). Then the game window is recorded into the replay buffer until it closes.
// With --desktop, the chosen monitor is recorded whenever no game is running.
// The hotkey saves the last N minutes of whatever is being recorded.

#include "background.h"
#include "game_detector.h"
#include "metrics.h"
#include "session.h"

#include <mfapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <sstream>

#pragma comment(lib, "shell32.lib")

using namespace wl;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_saveRequested{false};
std::atomic<bool> g_recordToggle{false};

BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

struct Hotkey {
    UINT modifiers = MOD_CONTROL | MOD_ALT;
    UINT vk = VK_F10;
    std::string text = "Ctrl+Alt+F10";
};

struct Options {
    bool list = false;
    bool desktop = false;
    int monitor = -1;  // desktop mode monitor, -1 = primary real monitor
    double minutes = 1.0;
    UINT fps = 60;
    UINT kbps = 15000;
    bool hevc = true;
    bool cursor = true;
    bool sound = true;
    std::wstring dir;
    Hotkey hotkey;
    Hotkey recordHotkey{MOD_CONTROL | MOD_ALT, VK_F9, "Ctrl+Alt+F9"};
    double splitMinutes = 0;
    int testRecordStart = -1, testRecordStop = -1;  // test aid: seconds after session start
    std::set<std::wstring> exclude = GameDetector::DefaultExcludes();
    std::set<std::wstring> include;
    int autoSaveSec = 0;   // test aid: save a clip N s after a session starts
    int exitAfterSec = 0;  // test aid: quit after N s
};

std::set<std::wstring> SplitList(const std::wstring& s) {
    std::set<std::wstring> out;
    std::wstringstream ss(s);
    std::wstring item;
    while (std::getline(ss, item, L',')) {
        item.erase(0, item.find_first_not_of(L' '));
        item.erase(item.find_last_not_of(L' ') + 1);
        if (item.size() > 4 && _wcsicmp(item.c_str() + item.size() - 4, L".exe") == 0) item.resize(item.size() - 4);
        if (!item.empty()) out.insert(item);
    }
    return out;
}

Hotkey ParseHotkey(const std::wstring& s) {
    Hotkey h;
    h.modifiers = 0;
    h.vk = 0;
    h.text = ToUtf8(s);
    std::wstring token;
    auto apply = [&](std::wstring t) {
        std::transform(t.begin(), t.end(), t.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (t == L"ctrl" || t == L"control") h.modifiers |= MOD_CONTROL;
        else if (t == L"alt") h.modifiers |= MOD_ALT;
        else if (t == L"shift") h.modifiers |= MOD_SHIFT;
        else if (t == L"win") h.modifiers |= MOD_WIN;
        else if (t.size() >= 2 && t[0] == L'f' && iswdigit(t[1])) h.vk = VK_F1 + std::stoi(t.substr(1)) - 1;
        else if (t.size() == 1 && iswalnum(t[0])) h.vk = static_cast<UINT>(towupper(t[0]));
        else throw std::runtime_error("Kisayol anlasilamadi: " + ToUtf8(t));
    };
    for (wchar_t c : s) {
        if (c == L'+') { apply(token); token.clear(); }
        else token += c;
    }
    if (!token.empty()) apply(token);
    if (!h.vk) throw std::runtime_error("Kisayolda tus yok: " + h.text);
    return h;
}

std::wstring DefaultClipDir() {
    wchar_t* videos = nullptr;
    std::wstring dir = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) dir = std::wstring(videos) + L"\\Waveline";
    CoTaskMemFree(videos);
    return dir;
}

void PrintUsage() {
    std::printf(
        "Waveline Clip - M2 (otomatik oyun algilama)\n\n"
        "  WavelineClip [secenekler]\n\n"
        "  --minutes M          geriye donuk sure, 0.5-10 dk (varsayilan 1)\n"
        "  --dir KLASOR         klip klasoru (varsayilan: Videolar\\Waveline)\n"
        "  --hotkey TUSLAR      klip kisayolu (varsayilan Ctrl+Alt+F10)\n"
        "  --record-hotkey T    manuel kayit baslat/durdur (varsayilan Ctrl+Alt+F9)\n"
        "  --split-minutes M    manuel kaydi M dakikada bir yeni dosyaya bol (varsayilan kapali)\n"
        "  --desktop            oyun yokken masaustunu de kaydet\n"
        "  --monitor N          masaustu modunda kaydedilecek monitor (--list ile bakin)\n"
        "  --include a,b        bu exe'ler pencereli olsa da oyun sayilir\n"
        "  --exclude a,b        bu exe'ler asla kaydedilmez (varsayilan listeye eklenir)\n"
        "  --list               monitorleri ve varsayilan haric listesini goster\n"
        "  --fps N | --bitrate KBPS | --codec hevc|h264 | --no-cursor | --no-sound\n"
        "  --auto-save SN | --exit-after SN | --test-record A:B   (test)\n\n"
        "Oyun tam ekran/borderless acildiginda kayit kendiliginden baslar. Ctrl+C ile cikis.\n");
}

Options ParseArgs(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) throw std::runtime_error("Eksik arguman: " + ToUtf8(a));
            return argv[++i];
        };
        if (a == L"--list") o.list = true;
        else if (a == L"--desktop") o.desktop = true;
        else if (a == L"--monitor") o.monitor = std::stoi(next());
        else if (a == L"--minutes") o.minutes = std::stod(next());
        else if (a == L"--dir") o.dir = next();
        else if (a == L"--hotkey") o.hotkey = ParseHotkey(next());
        else if (a == L"--include") o.include = SplitList(next());
        else if (a == L"--exclude") { for (auto& e : SplitList(next())) o.exclude.insert(e); }
        else if (a == L"--fps") o.fps = static_cast<UINT>(std::stoi(next()));
        else if (a == L"--bitrate") o.kbps = static_cast<UINT>(std::stoi(next()));
        else if (a == L"--codec") o.hevc = next() != L"h264";
        else if (a == L"--no-cursor") o.cursor = false;
        else if (a == L"--no-sound") o.sound = false;
        else if (a == L"--auto-save") o.autoSaveSec = std::stoi(next());
        else if (a == L"--record-hotkey") o.recordHotkey = ParseHotkey(next());
        else if (a == L"--split-minutes") o.splitMinutes = std::stod(next());
        else if (a == L"--test-record") {
            const std::wstring v = next();
            const size_t colon = v.find(L':');
            if (colon == std::wstring::npos) throw std::runtime_error("--test-record BASLA:BITIR (saniye)");
            o.testRecordStart = std::stoi(v.substr(0, colon));
            o.testRecordStop = std::stoi(v.substr(colon + 1));
        }
        else if (a == L"--exit-after") o.exitAfterSec = std::stoi(next());
        else if (a == L"--help" || a == L"-h" || a == L"/?") { PrintUsage(); std::exit(0); }
        else throw std::runtime_error("Bilinmeyen arguman: " + ToUtf8(a));
    }
    if (o.minutes < 0.5 || o.minutes > 10) throw std::runtime_error("--minutes 0.5 ile 10 arasinda olmali");
    if (o.fps < 1 || o.fps > 240) throw std::runtime_error("--fps 1-240 arasinda olmali");
    if (o.dir.empty()) o.dir = DefaultClipDir();
    return o;
}

class HotkeyListener {
public:
    // id 1 = save clip, id 2 = start/stop manual recording
    void Start(const Hotkey& clip, const Hotkey& record) {
        thread_ = std::thread([this, clip, record] {
            MSG msg;
            PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            threadId_ = GetCurrentThreadId();
            clipOk_ = RegisterHotKey(nullptr, 1, clip.modifiers | MOD_NOREPEAT, clip.vk) != FALSE;
            recordOk_ = RegisterHotKey(nullptr, 2, record.modifiers | MOD_NOREPEAT, record.vk) != FALSE;
            ready_ = true;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (msg.message != WM_HOTKEY) continue;
                if (msg.wParam == 1) g_saveRequested = true;
                else if (msg.wParam == 2) g_recordToggle = true;
            }
            if (clipOk_) UnregisterHotKey(nullptr, 1);
            if (recordOk_) UnregisterHotKey(nullptr, 2);
        });
        while (!ready_) Sleep(1);
    }
    void Stop() {
        if (threadId_) PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        if (thread_.joinable()) thread_.join();
    }
    bool ClipOk() const { return clipOk_; }
    bool RecordOk() const { return recordOk_; }

private:
    std::thread thread_;
    std::atomic<DWORD> threadId_{0};
    std::atomic<bool> ready_{false}, clipOk_{false}, recordOk_{false};
};

struct MonitorEntry {
    HMONITOR handle;
    std::wstring device;
    std::wstring adapter;
    RECT rect;
    bool primary;
    bool isVirtual;
};

std::vector<MonitorEntry> EnumerateMonitors() {
    std::vector<MonitorEntry> list;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(m, &mi);
            MonitorEntry e{m, mi.szDevice, L"", mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, false};
            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
                if (_wcsicmp(dd.DeviceName, mi.szDevice) == 0) {
                    e.adapter = dd.DeviceString;
                    break;
                }
                dd.cb = sizeof(dd);
            }
            // Virtual displays (Parsec, Sunshine/IDD...) are hidden from desktop mode by default.
            std::wstring a = e.adapter;
            std::transform(a.begin(), a.end(), a.begin(), towlower);
            e.isVirtual = a.find(L"parsec") != std::wstring::npos || a.find(L"virtual") != std::wstring::npos ||
                          a.find(L"indirect") != std::wstring::npos;
            reinterpret_cast<std::vector<MonitorEntry>*>(p)->push_back(e);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));
    return list;
}

const MonitorEntry* PickDesktopMonitor(const std::vector<MonitorEntry>& monitors, int index) {
    if (index >= 0) return static_cast<size_t>(index) < monitors.size() ? &monitors[static_cast<size_t>(index)] : nullptr;
    for (const auto& m : monitors)
        if (m.primary && !m.isVirtual) return &m;
    for (const auto& m : monitors)
        if (!m.isVirtual) return &m;
    return monitors.empty() ? nullptr : &monitors[0];
}

void RunList(const Options& opt) {
    std::printf("== Monitorler\n");
    const auto monitors = EnumerateMonitors();
    const MonitorEntry* desk = PickDesktopMonitor(monitors, opt.monitor);
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        std::printf("  [%zu] %s  %ldx%ld  %s%s%s%s\n", i, ToUtf8(m.device).c_str(), m.rect.right - m.rect.left,
                    m.rect.bottom - m.rect.top, ToUtf8(m.adapter).c_str(), m.primary ? "  (ana)" : "",
                    m.isVirtual ? "  (sanal, gizli)" : "", &m == desk ? "  <- masaustu modu" : "");
    }
    std::printf("\n== Oyun sayilmayan uygulamalar\n  ");
    for (const auto& e : opt.exclude) std::printf("%s ", ToUtf8(e).c_str());
    std::printf("\n");
}

int Run(const Options& opt) {
    std::filesystem::create_directories(opt.dir);

    GameDetector detector;
    detector.Configure(opt.exclude, opt.include);
    detector.Start();

    HotkeyListener hotkey;
    hotkey.Start(opt.hotkey, opt.recordHotkey);
    if (!hotkey.ClipOk()) Warn("%s kisayolu alinamadi (baska bir uygulama kullaniyor olabilir)", opt.hotkey.text.c_str());
    if (!hotkey.RecordOk())
        Warn("%s kisayolu alinamadi (baska bir uygulama kullaniyor olabilir)", opt.recordHotkey.text.c_str());

    // Log the audio routing once; every session reuses the same choice logic.
    {
        AudioEngine probe;
        SetupAudioSources(probe, true);
    }

    enum class Mode { Idle, Desktop, Game };
    Mode mode = Mode::Idle;
    HWND gameWindow = nullptr;
    std::unique_ptr<RecordingSession> session;
    int64_t sessionStart = 0, lastDesktopAttempt = 0;
    bool autoSaved = false, testRecordStarted = false, testRecordStopped = false;

    auto baseConfig = [&]() {
        SessionConfig c;
        c.dir = opt.dir;
        c.minutes = opt.minutes;
        c.fps = opt.fps;
        c.kbps = opt.kbps;
        c.hevc = opt.hevc;
        c.cursor = opt.cursor;
        c.sound = opt.sound;
        c.splitMinutes = opt.splitMinutes;
        return c;
    };
    auto stopSession = [&]() {
        if (session) {
            Log("Kayit durduruldu: %s", ToUtf8(session->Name()).c_str());
            session.reset();
        }
        mode = Mode::Idle;
        gameWindow = nullptr;
    };
    auto startSession = [&](SessionConfig c, Mode m) {
        try {
            auto s = std::make_unique<RecordingSession>(std::move(c));
            s->Start();
            session = std::move(s);
            mode = m;
            sessionStart = QpcNow100ns();
            autoSaved = testRecordStarted = testRecordStopped = false;
        } catch (const HrError& e) {
            Warn("Kayit baslatilamadi: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
        } catch (const winrt::hresult_error& e) {
            Warn("Kayit baslatilamadi: %s", ToUtf8(e.message().c_str()).c_str());
        } catch (const std::exception& e) {
            Warn("Kayit baslatilamadi: %s", e.what());
        }
    };

    Log("Hazir. Oyun bekleniyor%s. Klip: %s, kayit: %s, cikis: Ctrl+C. Klasor: %s",
        opt.desktop ? " (masaustu modu acik)" : "", opt.hotkey.text.c_str(), opt.recordHotkey.text.c_str(),
        ToUtf8(opt.dir).c_str());

    Metrics metrics;
    const int64_t runStart = QpcNow100ns();
    int64_t lastStatus = runStart;

    while (!g_stop) {
        Sleep(250);
        const int64_t now = QpcNow100ns();

        if (session && session->Ended()) stopSession();
        if (mode == Mode::Game && !IsWindow(gameWindow)) stopSession();

        // A game in the foreground (stable for 1.5 s) takes over from desktop/idle.
        if (mode != Mode::Game) {
            const GameCandidate game = detector.Stable(1500);
            if (game.hwnd) {
                stopSession();
                Log("Oyun algilandi: %s (%s)", ToUtf8(game.exe).c_str(), game.reason);
                SessionConfig c = baseConfig();
                c.name = game.exe;
                try {
                    c.item = CaptureSource::ItemForWindow(game.hwnd);
                    startSession(std::move(c), Mode::Game);
                    if (mode == Mode::Game) gameWindow = game.hwnd;
                } catch (const HrError& e) {
                    Warn("Oyun penceresi yakalanamadi: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
                }
            }
        }

        if (mode == Mode::Idle && opt.desktop && now - lastDesktopAttempt > 5 * kHns) {
            lastDesktopAttempt = now;
            const auto monitors = EnumerateMonitors();
            if (const MonitorEntry* m = PickDesktopMonitor(monitors, opt.monitor)) {
                SessionConfig c = baseConfig();
                c.name = L"Masaustu";
                c.item = CaptureSource::ItemForMonitor(m->handle);
                startSession(std::move(c), Mode::Desktop);
            }
        }

        // Test aids.
        if (opt.autoSaveSec > 0 && session && !autoSaved && now - sessionStart >= opt.autoSaveSec * kHns) {
            autoSaved = true;
            g_saveRequested = true;
        }
        if (opt.testRecordStart >= 0 && session) {
            const int64_t elapsed = now - sessionStart;
            if (!testRecordStarted && elapsed >= opt.testRecordStart * kHns) {
                testRecordStarted = true;
                g_recordToggle = true;
            } else if (testRecordStarted && !testRecordStopped && elapsed >= opt.testRecordStop * kHns) {
                testRecordStopped = true;
                g_recordToggle = true;
            }
        }
        if (opt.exitAfterSec > 0 && now - runStart >= opt.exitAfterSec * kHns) g_stop = true;

        if (g_saveRequested.exchange(false)) {
            if (session) session->RequestSave();
            else Warn("Su an kayit yapilmiyor (oyun yok, masaustu modu kapali)");
        }
        if (g_recordToggle.exchange(false)) {
            if (!session) Warn("Su an kayit yapilamaz (oyun yok, masaustu modu kapali)");
            else if (session->Recording()) session->StopRecording();
            else session->StartRecording();
        }

        if (now - lastStatus >= 10 * kHns) {
            lastStatus = now;
            metrics.Tick();
            const auto& m = metrics.Last();
            if (session) {
                char rec[48] = "";
                if (session->Recording()) {
                    const int s = static_cast<int>(session->RecordingSeconds());
                    snprintf(rec, sizeof(rec), " | KAYIT %02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
                }
                Log("Durum: %s%s | tampon %.0f sn (%.0f MB) | CPU %%%.1f | NVENC %%%.0f | RAM %.0f MB",
                    ToUtf8(session->Name()).c_str(), rec, session->BufferedSeconds(), session->BufferMB(), m.cpuPercent,
                    m.nvencPercent, m.workingSetMB);
            }
            else
                Log("Durum: bosta | CPU %%%.2f | RAM %.0f MB", m.cpuPercent, m.workingSetMB);
        }
    }

    stopSession();
    hotkey.Stop();
    detector.Stop();
    if (BackgroundTasks::Instance().Pending() > 0) Log("Kayit MP4'e donusturuluyor, bekleniyor...");
    BackgroundTasks::Instance().JoinAll();
    Log("Cikiliyor");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    try {
        const Options opt = ParseArgs(argc, argv);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Check(MFStartup(MF_VERSION), "MFStartup");
        const int rc = opt.list ? (RunList(opt), 0) : Run(opt);
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
