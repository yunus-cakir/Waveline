#include "game_detector.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace wl {

namespace {
GameDetector* g_detector = nullptr;  // WinEvent callbacks carry no context pointer

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

bool IsShellClass(HWND hwnd) {
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    static const wchar_t* shell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                     L"Windows.UI.Core.CoreWindow", L"ConsoleWindowClass",
                                     L"CASCADIA_HOSTING_WINDOW_CLASS"};
    for (const wchar_t* s : shell)
        if (wcscmp(cls, s) == 0) return true;
    return false;
}

// Window covers its monitor and has no title bar. DWM's frame bounds are used because a
// maximised normal window extends ~8 px past the monitor in GetWindowRect.
bool CoversMonitor(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;

    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}
}  // namespace

std::wstring ExeNameOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) name = Lower(std::filesystem::path(path).stem().wstring());
        CloseHandle(h);
    }
    return name;
}

std::set<std::wstring> GameDetector::DefaultExcludes() {
    return {
        // browsers
        L"chrome", L"msedge", L"firefox", L"brave", L"opera", L"opera_gx", L"vivaldi", L"min", L"iexplore",
        // media players / streaming
        L"vlc", L"mpc-hc64", L"mpc-hc", L"mpc-be64", L"potplayermini64", L"potplayer", L"wmplayer",
        L"microsoft.media.player", L"video.ui", L"spotify", L"applicationframehost", L"mpv", L"imageglass",
        // tools that are often fullscreen but are not games
        L"explorer", L"obs64", L"discord", L"steamwebhelper", L"parsecd", L"sunshine", L"powerpnt",
        L"devenv", L"code", L"photoshop", L"adobe premiere pro", L"resolve", L"losslesscut",
        // our own processes
        L"wavelineclip", L"wavelineclipproto",
    };
}

void GameDetector::Configure(std::set<std::wstring> exclude, std::set<std::wstring> include) {
    exclude_.clear();
    include_.clear();
    for (const auto& e : exclude) exclude_.insert(Lower(e));
    for (const auto& i : include) include_.insert(Lower(i));
}

bool GameDetector::IsGame(HWND hwnd, GameCandidate& out) const {
    if (!hwnd || IsShellClass(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;

    const std::wstring exe = ExeNameOf(hwnd);
    if (exe.empty()) return false;
    out.hwnd = hwnd;
    out.exe = exe;

    if (include_.count(exe)) {
        out.reason = "ekleme listesi";
        return IsWindowVisible(hwnd) && !IsIconic(hwnd);
    }
    if (exclude_.count(exe)) return false;

    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN) {
        out.reason = "exclusive fullscreen";
        return true;
    }
    if (CoversMonitor(hwnd)) {
        out.reason = "borderless/fullscreen";
        return true;
    }
    return false;
}

void GameDetector::Evaluate() {
    GameCandidate c;
    if (!IsGame(GetForegroundWindow(), c)) c = {};
    std::lock_guard lock(mutex_);
    if (c.hwnd != current_.hwnd) {
        current_ = c;
        since_ = QpcNow100ns();
    }
}

GameCandidate GameDetector::Stable(int stableMs) const {
    std::lock_guard lock(mutex_);
    if (!current_.hwnd || QpcNow100ns() - since_ < static_cast<int64_t>(stableMs) * 10'000) return {};
    if (!IsWindow(current_.hwnd)) return {};
    return current_;
}

void CALLBACK GameDetector::OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!g_detector || idObject != OBJID_WINDOW) return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE && hwnd != GetForegroundWindow()) return;
    g_detector->Evaluate();
}

void GameDetector::Start() {
    g_detector = this;
    thread_ = std::thread([this] {
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        threadId_ = GetCurrentThreadId();
        HWINEVENTHOOK fg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                                           WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        HWINEVENTHOOK loc = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent,
                                            0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        const UINT_PTR timer = SetTimer(nullptr, 0, 1000, nullptr);
        Evaluate();
        ready_ = true;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_TIMER) Evaluate();
            DispatchMessageW(&msg);
        }
        KillTimer(nullptr, timer);
        if (fg) UnhookWinEvent(fg);
        if (loc) UnhookWinEvent(loc);
    });
    while (!ready_) Sleep(1);
}

void GameDetector::Stop() {
    if (threadId_) PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
    g_detector = nullptr;
}

}  // namespace wl
