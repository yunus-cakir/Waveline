#include "common.h"

#include <cstdarg>
#include <functional>
#include <mutex>
#include <share.h>

namespace wl {

namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;

void VLog(const char* level, const char* fmt, va_list args) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);
    std::lock_guard lock(g_logMutex);
    std::printf("\r[%02u:%02u:%02u.%03u] %s%s\n", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, level, msg);
    std::fflush(stdout);
    if (g_logFile) {
        std::fprintf(g_logFile, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s%s\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                     st.wMinute, st.wSecond, st.wMilliseconds, level, msg);
        std::fflush(g_logFile);
    }
}
}  // namespace

void SetLogFile(const std::wstring& path) {
    std::lock_guard lock(g_logMutex);
    if (g_logFile) std::fclose(g_logFile);
    g_logFile = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

std::string ToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string ToUtf8(const std::wstring& w) { return ToUtf8(w.c_str()); }

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VLog("", fmt, args);
    va_end(args);
}

void Warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VLog("UYARI: ", fmt, args);
    va_end(args);
}

int64_t QpcNow100ns() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / freq) * kHns + (c.QuadPart % freq) * kHns / freq;
}

}  // namespace wl
