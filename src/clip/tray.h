#pragma once
#include "common.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace wl {

struct HotkeySpec {
    UINT modifiers = 0;
    UINT vk = 0;
    std::wstring text;
    bool Valid() const { return vk != 0; }
};
HotkeySpec ParseHotkey(const std::wstring& text);  // "Ctrl+Alt+F10" -> spec (vk 0 if invalid)

enum class TrayState { Idle, Buffering, Recording };

// The engine's only UI: a notification-area icon on its own thread with a hidden window.
// It owns the global hotkeys, the context menu and the notifications (shown by Windows 11
// as toasts; their own sound is muted, we play our chimes on the headphones instead).
class TrayApp {
public:
    struct Callbacks {
        std::function<void()> saveClip, toggleRecording, openSettings, openFolder, quit;
        std::function<void()> resumed;         // woke up from sleep/hibernate
        std::function<void()> displayChanged;  // monitor layout or resolution changed
    };

    static constexpr const wchar_t* kWindowClass = L"WavelineClipTray";

    void Start(Callbacks callbacks);
    void Stop();

    // Thread-safe; return whether each hotkey could be registered.
    std::pair<bool, bool> SetHotkeys(const HotkeySpec& clip, const HotkeySpec& record);
    void SetState(TrayState state, const std::wstring& tooltip);
    void Notify(const std::wstring& title, const std::wstring& text, const std::wstring& fileToSelect = {});

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void AddIcon();
    void UpdateIcon();
    void ShowMenu();

    Callbacks cb_;
    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr};
    UINT taskbarCreated_ = 0;

    std::mutex mutex_;  // guards the fields below, written by other threads
    TrayState state_ = TrayState::Idle;
    std::wstring tooltip_ = L"Waveline Clip";
    HotkeySpec clip_, record_;
    std::wstring notifyTitle_, notifyText_, lastFile_;
    std::pair<bool, bool> hotkeyResult_{false, false};
    HICON icons_[3] = {};
};

}  // namespace wl
