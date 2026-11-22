// Waveline Clip - background engine with tray icon (M4)
//
// Runs in the notification area. Idle until a game is detected (borderless/fullscreen
// foreground window, not a browser or media player); then the game is kept in the replay
// buffer until it closes. Optional desktop mode records a monitor when no game runs.
// Hotkeys: save clip / start-stop manual recording. Settings live in
// %APPDATA%\Waveline\clip.json (edited by WavelineSettings.exe) and apply live.

#include "background.h"
#include "clip_saver.h"
#include "recovery.h"
#include "config.h"
#include "game_detector.h"
#include "metrics.h"
#include "notify_sound.h"
#include "session.h"
#include "tray.h"

#include <mfapi.h>
#include <shellapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "shell32.lib")

using namespace wl;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_saveRequested{false};
std::atomic<bool> g_recordToggle{false};
std::atomic<bool> g_resumed{false};
std::atomic<bool> g_displayChanged{false};

BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

struct Options {
    bool console = false;
    bool list = false;
    int autoSaveSec = 0;   // test aids
    int exitAfterSec = 0;
    int testRecordStart = -1, testRecordStop = -1;
    std::wstring tagsFile;      // --tags FILE: print Waveline metadata
    std::wstring favoriteFile;  // --favorite FILE on|off: change only the tag
    bool favorite = true;
};

Options ParseArgs(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) throw std::runtime_error("Eksik arguman: " + ToUtf8(a));
            return argv[++i];
        };
        if (a == L"--console") o.console = true;
        else if (a == L"--list") o.list = o.console = true;
        else if (a == L"--auto-save") o.autoSaveSec = std::stoi(next());
        else if (a == L"--tags") o.tagsFile = next(), o.console = true;
        else if (a == L"--favorite") { o.favoriteFile = next(); o.favorite = next() != L"off"; o.console = true; }
        else if (a == L"--exit-after") o.exitAfterSec = std::stoi(next());
        else if (a == L"--test-record") {
            const std::wstring v = next();
            const size_t colon = v.find(L':');
            if (colon == std::wstring::npos) throw std::runtime_error("--test-record BASLA:BITIR");
            o.testRecordStart = std::stoi(v.substr(0, colon));
            o.testRecordStop = std::stoi(v.substr(colon + 1));
        }
        else throw std::runtime_error("Bilinmeyen arguman: " + ToUtf8(a));
    }
    return o;
}

struct MonitorEntry {
    HMONITOR handle;
    std::wstring device, adapter;
    RECT rect;
    bool primary, isVirtual;
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

const MonitorEntry* PickDesktopMonitor(const std::vector<MonitorEntry>& monitors, const std::wstring& device) {
    for (const auto& m : monitors)
        if (!device.empty() && _wcsicmp(m.device.c_str(), device.c_str()) == 0) return &m;
    for (const auto& m : monitors)
        if (m.primary && !m.isVirtual) return &m;
    for (const auto& m : monitors)
        if (!m.isVirtual) return &m;
    return monitors.empty() ? nullptr : &monitors[0];
}

// Monitors and output devices for the settings window (it has no Win32/audio code).
void WriteDevicesFile(NotifySound& sound) {
    using namespace winrt::Windows::Data::Json;
    JsonObject root;
    JsonArray mons;
    for (const auto& m : EnumerateMonitors()) {
        JsonObject o;
        o.SetNamedValue(L"device", JsonValue::CreateStringValue(m.device));
        o.SetNamedValue(L"adapter", JsonValue::CreateStringValue(m.adapter));
        o.SetNamedValue(L"width", JsonValue::CreateNumberValue(m.rect.right - m.rect.left));
        o.SetNamedValue(L"height", JsonValue::CreateNumberValue(m.rect.bottom - m.rect.top));
        o.SetNamedValue(L"primary", JsonValue::CreateBooleanValue(m.primary));
        o.SetNamedValue(L"virtual", JsonValue::CreateBooleanValue(m.isVirtual));
        mons.Append(o);
    }
    JsonArray outs;
    for (const auto& e : EnumerateEndpoints()) {
        if (e.capture || e.sonarRole != kSonarNone) continue;  // Sonar channels would be recorded
        JsonObject o;
        o.SetNamedValue(L"id", JsonValue::CreateStringValue(e.id));
        o.SetNamedValue(L"name", JsonValue::CreateStringValue(e.name));
        outs.Append(o);
    }
    root.SetNamedValue(L"monitors", mons);
    root.SetNamedValue(L"outputs", outs);
    root.SetNamedValue(L"autoNotifyDevice", JsonValue::CreateStringValue(sound.CurrentDeviceName()));
    JsonArray excl;
    for (const auto& e : GameDetector::DefaultExcludes()) excl.Append(JsonValue::CreateStringValue(e));
    root.SetNamedValue(L"defaultExcludes", excl);
    std::ofstream(AppDataDir() + L"\\devices.json", std::ios::binary) << ToUtf8(std::wstring(root.Stringify()));
}

int64_t FileTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<int64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

std::wstring FormatDuration(double seconds) {
    const int s = static_cast<int>(seconds);
    wchar_t buf[32];
    if (s >= 3600) swprintf_s(buf, L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else swprintf_s(buf, L"%d:%02d", s / 60, s % 60);
    return buf;
}

void OpenSettingsApp() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path(exe).parent_path();
    for (const auto& candidate : {dir / L"WavelineSettings.exe", dir / L"settings" / L"WavelineSettings.exe"}) {
        if (std::filesystem::exists(candidate)) {
            ShellExecuteW(nullptr, L"open", candidate.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", L"notepad.exe", ConfigPath().c_str(), nullptr, SW_SHOWNORMAL);  // fallback
}

int RunList() {
    std::printf("== Monitorler\n");
    for (const auto& m : EnumerateMonitors())
        std::printf("  %s  %ldx%ld  %s%s%s\n", ToUtf8(m.device).c_str(), m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
                    ToUtf8(m.adapter).c_str(), m.primary ? "  (ana)" : "", m.isVirtual ? "  (sanal)" : "");
    std::printf("\n== Ayar dosyasi: %s\n== Log: %s\n", ToUtf8(ConfigPath()).c_str(), ToUtf8(LocalDataDir() + L"\\clip.log").c_str());
    NotifySound s;
    std::printf("== Bildirim sesi cihazi (otomatik): %s\n", ToUtf8(s.CurrentDeviceName()).c_str());
    return 0;
}

// Prints (and optionally changes) the metadata Waveline stores in a clip.
int RunTags(const Options& opt) {
    const std::wstring path = opt.tagsFile.empty() ? opt.favoriteFile : opt.tagsFile;
    ClipMetadata m;
    if (!Mp4ReadMetadata(path, m)) {
        std::printf("Etiket okunamadi: %s\n", ToUtf8(path).c_str());
        return 1;
    }
    if (!opt.favoriteFile.empty()) {
        const std::string from = opt.favorite ? "\"favorite\":false" : "\"favorite\":true";
        const std::string to = opt.favorite ? "\"favorite\":true" : "\"favorite\":false";
        if (const size_t pos = m.editJson.find(from); pos != std::string::npos) m.editJson.replace(pos, from.size(), to);
        if (!Mp4WriteMetadata(path, m)) {
            std::printf("Etiket yazilamadi: %s\n", ToUtf8(path).c_str());
            return 1;
        }
        m = {};
        Mp4ReadMetadata(path, m);
    }
    std::printf("id      : %s\nkind    : %s\ngame    : %s\ntitle   : %s\ndate    : %s\ncomment : %s\nedit    : %s\n",
                ToUtf8(m.id).c_str(), ToUtf8(m.kind).c_str(), ToUtf8(m.game).c_str(), ToUtf8(m.title).c_str(),
                ToUtf8(m.date).c_str(), ToUtf8(m.comment).c_str(), m.editJson.c_str());
    return 0;
}
int Run(const Options& opt) {
    ClipConfig cfg = LoadConfig();
    std::wstring configPath = ConfigPath();
    int64_t configStamp = FileTime(configPath);
    auto clipDir = [&]() { return cfg.dir.empty() ? DefaultClipDir() : cfg.dir; };
    std::filesystem::create_directories(clipDir());
    if (cfg.autostart != AutostartEnabled()) SetAutostart(cfg.autostart);

    NotifySound sound;
    sound.Configure(cfg.sound, cfg.notifyDevice);
    WriteDevicesFile(sound);

    TrayApp tray;
    tray.Start({
        [] { g_saveRequested = true; },
        [] { g_recordToggle = true; },
        [] { OpenSettingsApp(); },
        [&] { ShellExecuteW(nullptr, L"open", clipDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); },
        [] { g_stop = true; },
        [] { g_resumed = true; },
        [] { g_displayChanged = true; },
    });
    auto applyHotkeys = [&]() {
        const auto [clipOk, recOk] = tray.SetHotkeys(ParseHotkey(cfg.clipHotkey), ParseHotkey(cfg.recordHotkey));
        if (!clipOk) Warn("Klip kisayolu alinamadi: %s", ToUtf8(cfg.clipHotkey).c_str());
        if (!recOk) Warn("Kayit kisayolu alinamadi: %s", ToUtf8(cfg.recordHotkey).c_str());
        if (!clipOk || !recOk)
            tray.Notify(L"KÄ±sayol kullanÄ±lamÄ±yor",
                        L"BaÅŸka bir uygulama kullanÄ±yor olabilir: " + (!clipOk ? cfg.clipHotkey : cfg.recordHotkey) +
                            L". Ayarlardan deÄŸiÅŸtirebilirsiniz.");
    };
    applyHotkeys();

    GameDetector detector;
    auto applyDetector = [&]() {
        auto excludes = GameDetector::DefaultExcludes();
        excludes.insert(cfg.exclude.begin(), cfg.exclude.end());
        detector.Configure(excludes, cfg.include);
    };
    applyDetector();
    detector.Start();

    {
        AudioEngine probe;  // log the audio routing once
        SetupAudioSources(probe, true);
    }

    // Session events arrive on worker threads: chime on the headphones + notification.
    std::mutex cfgMutex;  // cfg is replaced on reload while workers read it
    auto onEvent = [&](SessionEvent ev, const std::wstring& path, double seconds) {
        bool notifications;
        std::wstring recordHotkey;
        {
            std::lock_guard lock(cfgMutex);
            notifications = cfg.notifications;
            recordHotkey = cfg.recordHotkey;
        }
        const std::wstring file = std::filesystem::path(path).filename().wstring();
        switch (ev) {
            case SessionEvent::ClipSaved:
                sound.Play(Chime::ClipSaved);
                if (notifications) tray.Notify(L"Klip kaydedildi (" + FormatDuration(seconds) + L")", file, path);
                break;
            case SessionEvent::ClipFailed:
                sound.Play(Chime::Error);
                tray.Notify(L"Klip kaydedilemedi", L"AyrÄ±ntÄ±lar: " + LocalDataDir() + L"\\clip.log");
                break;
            case SessionEvent::RecordingStarted:
                sound.Play(Chime::RecordStart);
                if (notifications) tray.Notify(L"KayÄ±t baÅŸladÄ±", L"Durdurmak iÃ§in " + cfg.recordHotkey);
                break;
            case SessionEvent::RecordingStopped:
                sound.Play(Chime::RecordStop);
                break;
            case SessionEvent::DiskFull:
                sound.Play(Chime::Error);
                tray.Notify(L"Diskte yer kalmadı", L"Klip/kayıt durduruldu: " + path + L". Yer açın ya da ayarlardan başka bir klasör seçin.");
                break;
            case SessionEvent::RecordingFailed:
                sound.Play(Chime::Error);
                tray.Notify(L"Kayıt yazılamadı", L"Kayıt durduruldu, o ana kadarki kısım kaydedildi. Ayrıntılar: clip.log");
                break;
            case SessionEvent::RecordingReady:
                if (notifications) tray.Notify(L"KayÄ±t hazÄ±r", file, path);
                break;
        }
    };

    enum class Mode { Idle, Desktop, Game };
    Mode mode = Mode::Idle;
    HWND gameWindow = nullptr;
    std::unique_ptr<RecordingSession> session;
    int64_t sessionStart = 0, lastDesktopAttempt = 0;
    bool autoSaved = false, testRecordStarted = false, testRecordStopped = false;

    auto sessionConfig = [&](std::wstring name) {
        SessionConfig c;
        c.name = std::move(name);
        c.dir = clipDir();
        c.minutes = cfg.minutes;
        c.fps = cfg.fps;
        c.kbps = cfg.bitrateKbps;
        c.hevc = cfg.hevc;
        c.cursor = cfg.cursor;
        c.splitMinutes = cfg.splitMinutes;
        c.onEvent = onEvent;
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

    // Rebuild recordings interrupted by a crash or power loss (in the background).
    BackgroundTasks::Instance().Run([dir = clipDir(), &tray] {
        RecoverInterruptedRecordings(dir, [&tray](const std::wstring& path, double seconds) {
            tray.Notify(L"Yarım kalan kayıt kurtarıldı (" + FormatDuration(seconds) + L")",
                        std::filesystem::path(path).filename().wstring(), path);
        });
    });
    Log("Hazir. Klip: %s, kayit: %s. Klasor: %s", ToUtf8(cfg.clipHotkey).c_str(), ToUtf8(cfg.recordHotkey).c_str(),
        ToUtf8(clipDir()).c_str());

    Metrics metrics;
    const int64_t runStart = QpcNow100ns();
    int64_t lastStatus = runStart, lastConfigCheck = runStart;

    while (!g_stop) {
        Sleep(250);
        const int64_t now = QpcNow100ns();

        // --- live settings ---
        if (now - lastConfigCheck >= kHns) {
            lastConfigCheck = now;
            const int64_t stamp = FileTime(configPath);
            if (stamp != configStamp) {
                configStamp = stamp;
                const ClipConfig next = LoadConfig();
                const bool restart = next.SessionFieldsDiffer(cfg);
                const bool hotkeys = next.clipHotkey != cfg.clipHotkey || next.recordHotkey != cfg.recordHotkey;
                {
                    std::lock_guard lock(cfgMutex);
                    cfg = next;
                }
                Log("Ayarlar yeniden yuklendi%s", restart ? " (kayit yeniden baslatiliyor)" : "");
                std::filesystem::create_directories(clipDir());
                sound.Configure(cfg.sound, cfg.notifyDevice);
                applyDetector();
                if (hotkeys) applyHotkeys();
                if (cfg.autostart != AutostartEnabled()) SetAutostart(cfg.autostart);
                if (restart) {
                    if (session && session->Recording())
                        tray.Notify(L"Ayarlar uygulandÄ±", L"SÃ¼ren kayÄ±t kapatÄ±ldÄ± ve yeni ayarlarla devam ediliyor.");
                    stopSession();
                    lastDesktopAttempt = 0;
                }
                WriteDevicesFile(sound);
            }
        }

        // After sleep, capture and audio devices are often stale: start over cleanly.
        if (g_resumed.exchange(false)) {
            Log("Uykudan uyanildi, kayit yeniden baslatiliyor");
            stopSession();
            lastDesktopAttempt = 0;
        }
        // Monitor layout changed: the desktop capture may point at a moved/removed monitor.
        // (In a game, resolution changes are handled by the capture itself.)
        if (g_displayChanged.exchange(false) && mode == Mode::Desktop) {
            Log("Ekran duzeni degisti, masaustu kaydi yeniden baslatiliyor");
            stopSession();
            lastDesktopAttempt = 0;
        }        if (session && session->Ended()) stopSession();
        if (mode == Mode::Game && !IsWindow(gameWindow)) stopSession();

        if (mode != Mode::Game) {
            const GameCandidate game = detector.Stable(1500);
            if (game.hwnd) {
                stopSession();
                const std::wstring title = FriendlyAppName(game.hwnd);
                Log("Oyun algilandi: %s [%s] (%s)", ToUtf8(title).c_str(), ToUtf8(game.exe).c_str(), game.reason);
                try {
                    SessionConfig c = sessionConfig(title);
                    c.item = CaptureSource::ItemForWindow(game.hwnd);
                    startSession(std::move(c), Mode::Game);
                    if (mode == Mode::Game) gameWindow = game.hwnd;
                } catch (const HrError& e) {
                    Warn("Oyun penceresi yakalanamadi: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
                }
            }
        }

        if (mode == Mode::Idle && cfg.desktop && now - lastDesktopAttempt > 5 * kHns) {
            lastDesktopAttempt = now;
            const auto monitors = EnumerateMonitors();
            if (const MonitorEntry* m = PickDesktopMonitor(monitors, cfg.monitorDevice)) {
                SessionConfig c = sessionConfig(L"Masaüstü");
                c.item = CaptureSource::ItemForMonitor(m->handle);
                startSession(std::move(c), Mode::Desktop);
            }
        }

        // --- test aids ---
        if (opt.autoSaveSec > 0 && session && !autoSaved && now - sessionStart >= opt.autoSaveSec * kHns) {
            autoSaved = true;
            g_saveRequested = true;
        }
        if (opt.testRecordStart >= 0 && session) {
            const int64_t elapsed = now - sessionStart;
            if (!testRecordStarted && elapsed >= opt.testRecordStart * kHns) testRecordStarted = g_recordToggle = true;
            else if (testRecordStarted && !testRecordStopped && elapsed >= opt.testRecordStop * kHns)
                testRecordStopped = g_recordToggle = true;
        }
        if (opt.exitAfterSec > 0 && now - runStart >= opt.exitAfterSec * kHns) g_stop = true;

        // --- commands ---
        if (g_saveRequested.exchange(false)) {
            if (session) session->RequestSave();
            else tray.Notify(L"Åžu an kayÄ±t yapÄ±lmÄ±yor", L"Oyun algÄ±lanmadÄ± ve masaÃ¼stÃ¼ modu kapalÄ±.");
        }
        if (g_recordToggle.exchange(false)) {
            if (!session) tray.Notify(L"Åžu an kayÄ±t yapÄ±lamaz", L"Oyun algÄ±lanmadÄ± ve masaÃ¼stÃ¼ modu kapalÄ±.");
            else if (session->Recording()) session->StopRecording();
            else session->StartRecording();
        }

        // --- tray state (every second) and log status (every minute) ---
        if (session) {
            std::wstring tip = L"Waveline Clip â€“ " + session->Name() + L"\nTampon: " +
                               FormatDuration(session->BufferedSeconds());
            if (session->Recording()) tip += L"\nKayÄ±t: " + FormatDuration(session->RecordingSeconds());
            tray.SetState(session->Recording() ? TrayState::Recording : TrayState::Buffering, tip);
        } else {
            tray.SetState(TrayState::Idle, L"Waveline Clip â€“ oyun bekleniyor");
        }
        if (now - lastStatus >= (opt.console ? 10 : 60) * kHns) {
            lastStatus = now;
            metrics.Tick();
            const auto& m = metrics.Last();
            if (session)
                Log("Durum: %s%s | tampon %.0f sn (%.0f MB) | CPU %%%.1f | NVENC %%%.0f | RAM %.0f MB",
                    ToUtf8(session->Name()).c_str(), session->Recording() ? " | KAYIT" : "", session->BufferedSeconds(),
                    session->BufferMB(), m.cpuPercent, m.nvencPercent, m.workingSetMB);
            else
                Log("Durum: bosta | CPU %%%.2f | RAM %.0f MB", m.cpuPercent, m.workingSetMB);
        }
    }

    stopSession();
    detector.Stop();
    if (BackgroundTasks::Instance().Pending() > 0) Log("Arka plan isleri bekleniyor...");
    BackgroundTasks::Instance().JoinAll();
    tray.Stop();
    Log("Cikiliyor");
    return 0;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Options opt;
    try {
        opt = ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Waveline Clip", MB_ICONERROR);
        return 2;
    }
    if (opt.console) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // One engine per user session: a second start just opens the settings window.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WavelineClipEngine");
    const bool tool = opt.list || !opt.tagsFile.empty() || !opt.favoriteFile.empty();
    if (GetLastError() == ERROR_ALREADY_EXISTS && !tool) {
        OpenSettingsApp();
        return 0;
    }

    int rc = 1;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        SetLogFile(LocalDataDir() + L"\\clip.log");
        Check(MFStartup(MF_VERSION), "MFStartup");
        rc = opt.list ? RunList() : (!opt.tagsFile.empty() || !opt.favoriteFile.empty()) ? RunTags(opt) : Run(opt);
        MFShutdown();
    } catch (const HrError& e) {
        Warn("HATA: %s (HRESULT 0x%08X)", e.what(), static_cast<unsigned>(e.hr));
    } catch (const winrt::hresult_error& e) {
        Warn("HATA: %s", ToUtf8(e.message().c_str()).c_str());
    } catch (const std::exception& e) {
        Warn("HATA: %s", e.what());
    }
    if (mutex) CloseHandle(mutex);
    return rc;
}
