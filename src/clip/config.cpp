#include "config.h"

#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace winrt::Windows::Data::Json;

namespace wl {

namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    wchar_t* p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::string ReadFileUtf8(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) s.erase(0, 3);  // BOM
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

double Num(const JsonObject& o, const wchar_t* key, double def) {
    if (!o.HasKey(key)) return def;
    const auto v = o.GetNamedValue(key);
    return v.ValueType() == JsonValueType::Number ? v.GetNumber() : def;
}
bool Bool(const JsonObject& o, const wchar_t* key, bool def) {
    if (!o.HasKey(key)) return def;
    const auto v = o.GetNamedValue(key);
    return v.ValueType() == JsonValueType::Boolean ? v.GetBoolean() : def;
}
std::wstring Str(const JsonObject& o, const wchar_t* key, const std::wstring& def) {
    if (!o.HasKey(key)) return def;
    const auto v = o.GetNamedValue(key);
    return v.ValueType() == JsonValueType::String ? std::wstring(v.GetString()) : def;
}
std::set<std::wstring> StrSet(const JsonObject& o, const wchar_t* key) {
    std::set<std::wstring> out;
    if (!o.HasKey(key) || o.GetNamedValue(key).ValueType() != JsonValueType::Array) return out;
    for (const auto& v : o.GetNamedArray(key))
        if (v.ValueType() == JsonValueType::String) out.insert(std::wstring(v.GetString()));
    return out;
}
JsonArray ToArray(const std::set<std::wstring>& s) {
    JsonArray a;
    for (const auto& x : s) a.Append(JsonValue::CreateStringValue(x));
    return a;
}

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Waveline Clip";

}  // namespace

std::wstring AppDataDir() {
    std::wstring d = KnownFolder(FOLDERID_RoamingAppData) + L"\\Waveline";
    std::filesystem::create_directories(d);
    return d;
}
std::wstring LocalDataDir() {
    std::wstring d = KnownFolder(FOLDERID_LocalAppData) + L"\\Waveline";
    std::filesystem::create_directories(d);
    return d;
}
std::wstring ConfigPath() { return AppDataDir() + L"\\clip.json"; }
std::wstring DefaultClipDir() { return KnownFolder(FOLDERID_Videos) + L"\\Waveline"; }

ClipConfig LoadConfig() {
    ClipConfig c;
    const std::wstring path = ConfigPath();
    if (!std::filesystem::exists(path)) {
        SaveConfig(c);
        return c;
    }
    JsonObject o{nullptr};
    if (!JsonObject::TryParse(FromUtf8(ReadFileUtf8(path)), o)) {
        Warn("Ayar dosyasi okunamadi, varsayilanlar kullaniliyor: %s", ToUtf8(path).c_str());
        return c;
    }
    c.minutes = std::clamp(Num(o, L"minutes", c.minutes), 0.5, 10.0);
    c.dir = Str(o, L"dir", c.dir);
    c.clipHotkey = Str(o, L"clipHotkey", c.clipHotkey);
    c.recordHotkey = Str(o, L"recordHotkey", c.recordHotkey);
    c.desktop = Bool(o, L"desktop", c.desktop);
    c.monitorDevice = Str(o, L"monitorDevice", c.monitorDevice);
    c.fps = static_cast<UINT>(std::clamp(Num(o, L"fps", c.fps), 10.0, 240.0));
    c.bitrateKbps = static_cast<UINT>(std::clamp(Num(o, L"bitrateKbps", c.bitrateKbps), 1000.0, 150000.0));
    c.hevc = Str(o, L"codec", L"hevc") != L"h264";
    c.cursor = Bool(o, L"cursor", c.cursor);
    c.splitMinutes = std::max(0.0, Num(o, L"splitMinutes", c.splitMinutes));
    c.sound = Bool(o, L"sound", c.sound);
    c.notifications = Bool(o, L"notifications", c.notifications);
    c.notifyDevice = Str(o, L"notifyDevice", c.notifyDevice);
    c.include = StrSet(o, L"include");
    c.exclude = StrSet(o, L"exclude");
    c.autostart = Bool(o, L"autostart", c.autostart);
    return c;
}

void SaveConfig(const ClipConfig& c) {
    JsonObject o;
    o.SetNamedValue(L"minutes", JsonValue::CreateNumberValue(c.minutes));
    o.SetNamedValue(L"dir", JsonValue::CreateStringValue(c.dir));
    o.SetNamedValue(L"clipHotkey", JsonValue::CreateStringValue(c.clipHotkey));
    o.SetNamedValue(L"recordHotkey", JsonValue::CreateStringValue(c.recordHotkey));
    o.SetNamedValue(L"desktop", JsonValue::CreateBooleanValue(c.desktop));
    o.SetNamedValue(L"monitorDevice", JsonValue::CreateStringValue(c.monitorDevice));
    o.SetNamedValue(L"fps", JsonValue::CreateNumberValue(c.fps));
    o.SetNamedValue(L"bitrateKbps", JsonValue::CreateNumberValue(c.bitrateKbps));
    o.SetNamedValue(L"codec", JsonValue::CreateStringValue(c.hevc ? L"hevc" : L"h264"));
    o.SetNamedValue(L"cursor", JsonValue::CreateBooleanValue(c.cursor));
    o.SetNamedValue(L"splitMinutes", JsonValue::CreateNumberValue(c.splitMinutes));
    o.SetNamedValue(L"sound", JsonValue::CreateBooleanValue(c.sound));
    o.SetNamedValue(L"notifications", JsonValue::CreateBooleanValue(c.notifications));
    o.SetNamedValue(L"notifyDevice", JsonValue::CreateStringValue(c.notifyDevice));
    o.SetNamedValue(L"include", ToArray(c.include));
    o.SetNamedValue(L"exclude", ToArray(c.exclude));
    o.SetNamedValue(L"autostart", JsonValue::CreateBooleanValue(c.autostart));
    std::ofstream(ConfigPath(), std::ios::binary) << ToUtf8(std::wstring(o.Stringify()));
}

void SetAutostart(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

bool AutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    const bool exists = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

}  // namespace wl
