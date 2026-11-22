#pragma once
#include "common.h"

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace wl {

struct GameCandidate {
    HWND hwnd = nullptr;
    std::wstring exe;     // lower-case, without ".exe"
    const char* reason = "";
};

// Finds the game the user is playing, without touching the game process:
// - event driven: SetWinEventHook (out-of-context, no DLL injection) for foreground and
//   move/resize of the foreground window, plus a 1 s timer for late fullscreen switches
// - a window is a game when it is the foreground window and covers its whole monitor
//   without a title bar (borderless / fullscreen), or the system reports exclusive D3D
//   fullscreen, or its exe is on the include list
// - browsers, media players and the shell are excluded (YouTube fullscreen is borderless too)
class GameDetector {
public:
    void Configure(std::set<std::wstring> exclude, std::set<std::wstring> include);
    void Start();
    void Stop();

    // The current candidate if it has been stable for at least `stableMs`.
    GameCandidate Stable(int stableMs) const;

    static std::set<std::wstring> DefaultExcludes();

private:
    void Evaluate();
    bool IsGame(HWND hwnd, GameCandidate& out) const;
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    std::set<std::wstring> exclude_, include_;
    std::thread thread_;
    std::atomic<DWORD> threadId_{0};
    std::atomic<bool> ready_{false};

    mutable std::mutex mutex_;
    GameCandidate current_;
    int64_t since_ = 0;
};

// Lower-case exe name (no extension) of a window's process. Uses only
// PROCESS_QUERY_LIMITED_INFORMATION (anti-cheat rule 1).
std::wstring ExeNameOf(HWND hwnd);
std::wstring ExePathOf(HWND hwnd);
// Human-readable name for folders and tags, e.g. "Counter-Strike 2" instead of "cs2".
std::wstring FriendlyAppName(HWND hwnd);

}  // namespace wl
