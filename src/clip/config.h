#pragma once
#include "common.h"

#include <set>
#include <string>

namespace wl {

// User settings, stored as JSON in %APPDATA%\Waveline\clip.json. The settings window
// edits this file; the engine watches it and applies changes live.
struct ClipConfig {
    double minutes = 1.0;
    std::wstring dir;  // empty = Videos\Waveline
    std::wstring clipHotkey = L"Ctrl+Alt+F10";
    std::wstring recordHotkey = L"Ctrl+Alt+F9";
    bool desktop = false;
    std::wstring monitorDevice;  // e.g. \\.\DISPLAY2; empty = primary real monitor
    UINT fps = 60;
    UINT bitrateKbps = 15000;
    bool hevc = true;
    bool cursor = true;
    double splitMinutes = 0;
    bool sound = true;
    bool notifications = true;
    std::wstring notifyDevice = L"auto";  // "auto" = Sonar's headphone device, else endpoint id
    std::set<std::wstring> include;
    std::set<std::wstring> exclude;  // added to the built-in list
    bool autostart = false;

    // Fields whose change requires restarting the running capture session.
    bool SessionFieldsDiffer(const ClipConfig& o) const {
        return fps != o.fps || bitrateKbps != o.bitrateKbps || hevc != o.hevc || cursor != o.cursor ||
               desktop != o.desktop || monitorDevice != o.monitorDevice || dir != o.dir ||
               splitMinutes != o.splitMinutes || minutes != o.minutes;
    }
};

std::wstring AppDataDir();      // %APPDATA%\Waveline (created)
std::wstring ConfigPath();      // ...\clip.json
std::wstring LocalDataDir();    // %LOCALAPPDATA%\Waveline (created), logs
std::wstring DefaultClipDir();  // Videos\Waveline

ClipConfig LoadConfig();  // missing/invalid file -> defaults (and writes them)
void SaveConfig(const ClipConfig& c);

// Windows "start with Windows" (HKCU Run key).
void SetAutostart(bool enabled);
bool AutostartEnabled();

}  // namespace wl
