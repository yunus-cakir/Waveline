// Waveline Clip - M0 prototype
// Shared helpers: logging, HRESULT checks, clock.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <unknwn.h>
#include <inspectable.h>
#include <winrt/base.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace wl {

using winrt::com_ptr;

struct HrError : std::runtime_error {
    HRESULT hr;
    HrError(HRESULT h, const char* what) : std::runtime_error(what), hr(h) {}
};

inline void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw HrError(hr, what);
}

std::string ToUtf8(const std::wstring& w);
std::string ToUtf8(const wchar_t* w);

void Log(const char* fmt, ...);
void Warn(const char* fmt, ...);
void SetLogFile(const std::wstring& path);  // also write the log to a file

// QueryPerformanceCounter converted to 100 ns units. This is the same time base
// WASAPI reports in IAudioCaptureClient::GetBuffer (u64QPCPosition), so audio and
// video share one clock.
int64_t QpcNow100ns();

constexpr int64_t kHns = 10'000'000;      // 100 ns ticks per second
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;

inline int64_t FramesToHns(int64_t frames) { return frames * kHns / kSampleRate; }

}  // namespace wl
