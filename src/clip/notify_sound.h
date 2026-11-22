#pragma once
#include "common.h"

namespace wl {

enum class Chime { ClipSaved, RecordStart, RecordStop, Error };

// Short synthesized chimes played straight to the *physical* headphone device, never to a
// Sonar virtual channel, so they don't end up in the Game track of the next clip.
// "auto": asks Sonar's local API which device its Personal (monitoring) mix plays on;
// without Sonar, the default output device.
class NotifySound {
public:
    void Configure(bool enabled, const std::wstring& deviceSetting);
    void Play(Chime chime);
    std::wstring CurrentDeviceName();

private:
    std::wstring ResolveDevice();

    bool enabled_ = true;
    std::wstring setting_ = L"auto";
    std::wstring cachedId_;
    int64_t cachedAt_ = 0;
};

// Device id of Sonar's monitoring (Personal Mix) output, or empty if Sonar is not running.
std::wstring QuerySonarMonitoringDevice();

}  // namespace wl
