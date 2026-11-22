#include "tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace wl {

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;     // icon mouse events
constexpr UINT kUpdateMsg = WM_APP + 2;   // state/tooltip changed
constexpr UINT kNotifyMsg = WM_APP + 3;   // show notification
constexpr UINT kHotkeyMsg = WM_APP + 4;   // (re)register hotkeys
constexpr UINT kTrayId = 1;
enum MenuId : UINT { kMenuSave = 100, kMenuRecord, kMenuFolder, kMenuSettings, kMenuQuit };

// Draws the tray icon: a ring with three "wave" bars, coloured by state.
HICON MakeIcon(COLORREF color, bool filled) {
    const int size = GetSystemMetrics(SM_CXSMICON) * 2;  // drawn at 2x, Windows scales
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color32 = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    auto* px = static_cast<uint32_t*>(bits);
    const uint32_t r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    const double c = (size - 1) / 2.0, outer = size / 2.0 - 1, inner = outer - size / 9.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double d = std::hypot(x - c, y - c);
            double a = 0;
            if (d <= outer) a = std::clamp(outer - d, 0.0, 1.0);
            if (!filled && d < inner) a = std::clamp(d - (inner - 1), 0.0, 1.0);
            // three vertical bars in the middle (a tiny waveform)
            const double bw = size / 10.0;
            const double heights[3] = {0.28, 0.46, 0.28};
            for (int i = 0; i < 3; ++i) {
                const double bx = c + (i - 1) * bw * 1.8;
                if (std::abs(x - bx) <= bw / 2 && std::abs(y - c) <= heights[i] * size / 2) a = 1;
            }
            if (filled && a > 0 && d < inner) {  // bars cut out of a filled disc
                for (int i = 0; i < 3; ++i) {
                    const double bx = c + (i - 1) * bw * 1.8;
                    if (std::abs(x - bx) <= bw / 2 && std::abs(y - c) <= heights[i] * size / 2) a = 0;
                }
            }
            const uint32_t A = static_cast<uint32_t>(a * 255);
            px[y * size + x] = (A << 24) | ((r * A / 255) << 16) | ((g * A / 255) << 8) | (b * A / 255);
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color32};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color32);
    return icon;
}

}  // namespace

HotkeySpec ParseHotkey(const std::wstring& text) {
    HotkeySpec h;
    h.text = text;
    std::wstring token;
    auto apply = [&](std::wstring t) {
        t.erase(0, t.find_first_not_of(L' '));
        t.erase(t.find_last_not_of(L' ') + 1);
        std::transform(t.begin(), t.end(), t.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        if (t == L"ctrl" || t == L"control") h.modifiers |= MOD_CONTROL;
        else if (t == L"alt") h.modifiers |= MOD_ALT;
        else if (t == L"shift") h.modifiers |= MOD_SHIFT;
        else if (t == L"win") h.modifiers |= MOD_WIN;
        else if (t.size() >= 2 && t[0] == L'f' && iswdigit(t[1])) h.vk = VK_F1 + _wtoi(t.c_str() + 1) - 1;
        else if (t.size() == 1 && iswalnum(t[0])) h.vk = static_cast<UINT>(towupper(t[0]));
        else if (t == L"printscreen" || t == L"prtsc") h.vk = VK_SNAPSHOT;
        else if (t == L"pause") h.vk = VK_PAUSE;
        else if (t == L"insert") h.vk = VK_INSERT;
        else if (t == L"home") h.vk = VK_HOME;
        else if (t == L"end") h.vk = VK_END;
        else if (t.rfind(L"num", 0) == 0 && t.size() == 4 && iswdigit(t[3])) h.vk = VK_NUMPAD0 + (t[3] - L'0');
    };
    for (wchar_t ch : text) {
        if (ch == L'+') { apply(token); token.clear(); }
        else token += ch;
    }
    if (!token.empty()) apply(token);
    return h;
}

void TrayApp::Start(Callbacks callbacks) {
    cb_ = std::move(callbacks);
    icons_[0] = MakeIcon(RGB(140, 140, 150), false);  // idle
    icons_[1] = MakeIcon(RGB(32, 201, 151), false);   // buffering
    icons_[2] = MakeIcon(RGB(235, 64, 52), true);     // recording

    std::atomic<bool> ready{false};
    thread_ = std::thread([this, &ready] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TrayApp::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);
        // A hidden top-level window (not message-only): it must receive the TaskbarCreated
        // broadcast and be findable by a second instance.
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Waveline Clip", WS_POPUP, 0, 0, 0, 0, nullptr,
                                    nullptr, wc.hInstance, this);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        hwnd_ = hwnd;
        AddIcon();
        ready = true;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        UnregisterHotKey(hwnd, 1);
        UnregisterHotKey(hwnd, 2);
        DestroyWindow(hwnd);
    });
    while (!ready) Sleep(1);
}

void TrayApp::Stop() {
    if (HWND h = hwnd_.load()) PostMessageW(h, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
    for (HICON& i : icons_)
        if (i) DestroyIcon(i), i = nullptr;
}

std::pair<bool, bool> TrayApp::SetHotkeys(const HotkeySpec& clip, const HotkeySpec& record) {
    {
        std::lock_guard lock(mutex_);
        clip_ = clip;
        record_ = record;
    }
    SendMessageW(hwnd_, kHotkeyMsg, 0, 0);  // hotkeys belong to the window's thread
    std::lock_guard lock(mutex_);
    return hotkeyResult_;
}

void TrayApp::SetState(TrayState state, const std::wstring& tooltip) {
    {
        std::lock_guard lock(mutex_);
        if (state == state_ && tooltip == tooltip_) return;
        state_ = state;
        tooltip_ = tooltip;
    }
    PostMessageW(hwnd_, kUpdateMsg, 0, 0);
}

void TrayApp::Notify(const std::wstring& title, const std::wstring& text, const std::wstring& fileToSelect) {
    {
        std::lock_guard lock(mutex_);
        notifyTitle_ = title;
        notifyText_ = text;
        lastFile_ = fileToSelect;
    }
    PostMessageW(hwnd_, kNotifyMsg, 0, 0);
}

void TrayApp::AddIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    {
        std::lock_guard lock(mutex_);
        nid.hIcon = icons_[static_cast<int>(state_)];
        wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    }
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayApp::UpdateIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    {
        std::lock_guard lock(mutex_);
        nid.hIcon = icons_[static_cast<int>(state_)];
        wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::ShowMenu() {
    TrayState state;
    std::wstring clipKey, recKey;
    {
        std::lock_guard lock(mutex_);
        state = state_;
        clipKey = clip_.text;
        recKey = record_.text;
    }
    HMENU menu = CreatePopupMenu();
    const UINT active = state == TrayState::Idle ? MF_GRAYED : 0;
    AppendMenuW(menu, MF_STRING | active, kMenuSave, (L"Klip kaydet\t" + clipKey).c_str());
    AppendMenuW(menu, MF_STRING | active, kMenuRecord,
                ((state == TrayState::Recording ? L"Kaydı durdur\t" : L"Kaydı başlat\t") + recKey).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuFolder, L"Klasörü aç");
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Ayarlar…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Çıkış");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);  // so the menu closes when clicking elsewhere
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case kMenuSave: if (cb_.saveClip) cb_.saveClip(); break;
        case kMenuRecord: if (cb_.toggleRecording) cb_.toggleRecording(); break;
        case kMenuFolder: if (cb_.openFolder) cb_.openFolder(); break;
        case kMenuSettings: if (cb_.openSettings) cb_.openSettings(); break;
        case kMenuQuit: if (cb_.quit) cb_.quit(); break;
        default: break;
    }
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreated_ && taskbarCreated_) {  // Explorer restarted
        AddIcon();
        return 0;
    }
    switch (msg) {
        case kTrayMsg:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP: ShowMenu(); break;
                case WM_LBUTTONDBLCLK:
                case NIN_SELECT: if (cb_.openSettings) cb_.openSettings(); break;
                case NIN_BALLOONUSERCLICK: {
                    std::wstring file;
                    {
                        std::lock_guard lock(mutex_);
                        file = lastFile_;
                    }
                    if (!file.empty())
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + file + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                    else if (cb_.openFolder)
                        cb_.openFolder();
                    break;
                }
                default: break;
            }
            return 0;
        case kUpdateMsg: UpdateIcon(); return 0;
        case kNotifyMsg: {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd_;
            nid.uID = kTrayId;
            nid.uFlags = NIF_INFO;
            nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_NOSOUND;
            {
                std::lock_guard lock(mutex_);
                wcsncpy_s(nid.szInfoTitle, notifyTitle_.c_str(), _TRUNCATE);
                wcsncpy_s(nid.szInfo, notifyText_.c_str(), _TRUNCATE);
                nid.hBalloonIcon = icons_[static_cast<int>(state_ == TrayState::Recording ? 2 : 1)];
            }
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            return 0;
        }
        case kHotkeyMsg: {
            UnregisterHotKey(hwnd_, 1);
            UnregisterHotKey(hwnd_, 2);
            std::lock_guard lock(mutex_);
            hotkeyResult_.first = clip_.Valid() && RegisterHotKey(hwnd_, 1, clip_.modifiers | MOD_NOREPEAT, clip_.vk);
            hotkeyResult_.second = record_.Valid() && RegisterHotKey(hwnd_, 2, record_.modifiers | MOD_NOREPEAT, record_.vk);
            return 0;
        }
        case WM_HOTKEY:
            if (wp == 1 && cb_.saveClip) cb_.saveClip();
            if (wp == 2 && cb_.toggleRecording) cb_.toggleRecording();
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace wl
