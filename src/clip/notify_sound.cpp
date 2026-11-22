#include "notify_sound.h"
#include "audio.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

using namespace winrt::Windows::Data::Json;

namespace wl {

namespace {

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Minimal local HTTP(S) GET. Only used against 127.0.0.1 (SteelSeries GG / Sonar), whose
// HTTPS endpoint uses a self-signed certificate.
std::string HttpGet(const std::wstring& url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return {};
    if (_wcsicmp(host, L"127.0.0.1") != 0 && _wcsicmp(host, L"localhost") != 0) return {};  // local only

    std::string body;
    HINTERNET session = WinHttpOpen(L"Waveline", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return {};
    WinHttpSetTimeouts(session, 500, 500, 1000, 1000);
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, nullptr, nullptr, https ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    if (req) {
        if (https) {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        }
        if (WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
                body.append(chunk.data(), read);
            }
        }
        WinHttpCloseHandle(req);
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}

std::vector<float> Synthesize(Chime chime) {
    struct Note { double hz; double ms; };
    std::vector<Note> notes;
    switch (chime) {
        case Chime::ClipSaved: notes = {{988, 70}, {1319, 110}}; break;
        case Chime::RecordStart: notes = {{659, 80}, {880, 80}, {1175, 110}}; break;
        case Chime::RecordStop: notes = {{1175, 80}, {880, 80}, {659, 110}}; break;
        case Chime::Error: notes = {{330, 160}, {262, 200}}; break;
    }
    std::vector<float> out;  // interleaved stereo
    for (const auto& n : notes) {
        const size_t len = static_cast<size_t>(kSampleRate * n.ms / 1000.0);
        for (size_t i = 0; i < len; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            const double fade = std::min(1.0, std::min(i / 240.0, (len - i) / 1200.0));  // click-free
            const float v = static_cast<float>(0.18 * fade * std::sin(2.0 * 3.14159265358979 * n.hz * t));
            out.push_back(v);
            out.push_back(v);
        }
    }
    return out;
}

void PlayOn(const std::wstring& deviceId, std::vector<float> pcm) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    com_ptr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(en.put())))) return;
    com_ptr<IMMDevice> dev;
    if (FAILED(en->GetDevice(deviceId.c_str(), dev.put()))) return;
    com_ptr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) return;

    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = 2;
    wf.Format.nSamplesPerSec = kSampleRate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = 8;
    wf.Format.nAvgBytesPerSec = kSampleRate * 8;
    wf.Format.cbSize = sizeof(wf) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2'000'000, 0, &wf.Format, nullptr))) return;
    com_ptr<IAudioRenderClient> render;
    if (FAILED(client->GetService(IID_PPV_ARGS(render.put())))) return;
    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    const UINT32 total = static_cast<UINT32>(pcm.size() / 2);
    UINT32 written = 0;
    client->Start();
    while (written < total) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        const UINT32 n = std::min(bufferFrames - padding, total - written);
        if (n > 0) {
            BYTE* dst = nullptr;
            if (SUCCEEDED(render->GetBuffer(n, &dst))) {
                memcpy(dst, pcm.data() + written * 2, n * 8);
                render->ReleaseBuffer(n, 0);
                written += n;
            }
        }
        Sleep(10);
    }
    Sleep(static_cast<DWORD>(bufferFrames * 1000 / kSampleRate / 2 + 50));  // let it drain
    client->Stop();
}

}  // namespace

std::wstring QuerySonarMonitoringDevice() {
    // GG publishes its local addresses in coreProps.json; Sonar's own web server address
    // (port changes on every start) comes from GG's /subApps.
    std::ifstream f(L"C:\\ProgramData\\SteelSeries\\GG\\coreProps.json", std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    JsonObject props{nullptr};
    if (!JsonObject::TryParse(FromUtf8(ss.str()), props) || !props.HasKey(L"ggEncryptedAddress")) return {};

    JsonObject sub{nullptr};
    const std::wstring ggUrl = L"https://" + std::wstring(props.GetNamedString(L"ggEncryptedAddress")) + L"/subApps";
    if (!JsonObject::TryParse(FromUtf8(HttpGet(ggUrl)), sub)) return {};
    try {
        const std::wstring sonar = std::wstring(
            sub.GetNamedObject(L"subApps").GetNamedObject(L"sonar").GetNamedObject(L"metadata").GetNamedString(L"webServerAddress"));
        JsonArray redirections{nullptr};
        if (!JsonArray::TryParse(FromUtf8(HttpGet(sonar + L"/streamRedirections")), redirections)) return {};
        for (const auto& v : redirections) {
            if (v.ValueType() != JsonValueType::Object) continue;
            const auto o = v.GetObject();
            if (o.GetNamedString(L"streamRedirectionId", L"") == L"monitoring") return std::wstring(o.GetNamedString(L"deviceId", L""));
        }
    } catch (...) {
    }
    return {};
}

void NotifySound::Configure(bool enabled, const std::wstring& deviceSetting) {
    enabled_ = enabled;
    if (deviceSetting != setting_) cachedAt_ = 0;
    setting_ = deviceSetting.empty() ? L"auto" : deviceSetting;
}

std::wstring NotifySound::ResolveDevice() {
    const int64_t now = QpcNow100ns();
    if (cachedAt_ && now - cachedAt_ < 60 * kHns) return cachedId_;

    const auto endpoints = EnumerateEndpoints();
    auto active = [&](const std::wstring& id) {
        for (const auto& e : endpoints)
            if (!e.capture && e.id == id) return true;
        return false;
    };
    std::wstring id;
    if (setting_ != L"auto" && active(setting_)) id = setting_;
    if (id.empty()) {
        const std::wstring sonar = QuerySonarMonitoringDevice();
        if (!sonar.empty() && active(sonar)) id = sonar;
    }
    if (id.empty()) {  // default output, unless it is a Sonar channel (would be recorded)
        const std::wstring def = DefaultEndpointId(false);
        for (const auto& e : endpoints)
            if (!e.capture && e.id == def && e.sonarRole == kSonarNone) id = def;
    }
    cachedId_ = id;
    cachedAt_ = now;
    return id;
}

std::wstring NotifySound::CurrentDeviceName() {
    const std::wstring id = ResolveDevice();
    for (const auto& e : EnumerateEndpoints())
        if (e.id == id) return e.name;
    return L"(yok)";
}

void NotifySound::Play(Chime chime) {
    if (!enabled_) return;
    const std::wstring id = ResolveDevice();
    if (id.empty()) return;  // only a Sonar channel available: stay silent rather than pollute clips
    std::thread(PlayOn, id, Synthesize(chime)).detach();
}

}  // namespace wl
