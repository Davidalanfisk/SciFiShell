#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "OsSettings.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <powrprof.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace sf::os {

namespace {
// Opens the default render endpoint's volume interface; the caller releases it.
IAudioEndpointVolume* openEndpoint() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // harmless when the thread is already initialised
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))))
        return nullptr;
    IMMDevice* dev = nullptr;
    IAudioEndpointVolume* vol = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)))
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&vol));
    if (dev) dev->Release();
    en->Release();
    return vol;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string regString(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[256] = L"";
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return utf8(buf);
}

constexpr const wchar_t* kPersonalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

std::string gb(double bytes) {
    char b[32];
    std::snprintf(b, sizeof b, bytes >= 1e11 ? "%.0f GB" : "%.1f GB", bytes / 1073741824.0);
    return b;
}
}  // namespace

float getVolume() {
    IAudioEndpointVolume* v = openEndpoint();
    if (!v) return -1.0f;
    float f = -1.0f;
    if (FAILED(v->GetMasterVolumeLevelScalar(&f))) f = -1.0f;
    v->Release();
    return f;
}

void setVolume(float f) {
    if (IAudioEndpointVolume* v = openEndpoint()) {
        v->SetMasterVolumeLevelScalar(std::clamp(f, 0.0f, 1.0f), nullptr);
        v->Release();
    }
}

bool getMute() {
    IAudioEndpointVolume* v = openEndpoint();
    if (!v) return false;
    BOOL m = FALSE;
    v->GetMute(&m);
    v->Release();
    return m != FALSE;
}

void setMute(bool m) {
    if (IAudioEndpointVolume* v = openEndpoint()) {
        v->SetMute(m ? TRUE : FALSE, nullptr);
        v->Release();
    }
}

int mouseSpeed() {
    int v = 10;
    SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &v, 0);
    return v;
}

void setMouseSpeed(int v) {
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(std::clamp(v, 1, 20))),
                          SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

bool mouseSwapped() { return GetSystemMetrics(SM_SWAPBUTTON) != 0; }
void setMouseSwapped(bool swapped) { SwapMouseButton(swapped ? TRUE : FALSE); }

bool darkMode() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

void setDarkMode(bool dark) {
    const DWORD v = dark ? 0 : 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", REG_DWORD, &v, sizeof(v));
    RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalize, L"SystemUsesLightTheme", REG_DWORD, &v, sizeof(v));
    DWORD_PTR res = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 200, &res);
}

void lockPc() { LockWorkStation(); }
void sleepPc() { SetSuspendState(FALSE, FALSE, FALSE); }

void powerOff(bool reboot) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueA(nullptr, "SeShutdownPrivilege", &tp.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    ExitWindowsEx((reboot ? EWX_REBOOT : EWX_SHUTDOWN) | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED);
}

std::string batteryText() {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s) || s.BatteryFlag == 128) return "MAINS POWER";
    std::string t = s.BatteryLifePercent <= 100 ? std::to_string(s.BatteryLifePercent) + "%" : "?";
    if (s.ACLineStatus == 1) t += " CHARGING";
    return t;
}

std::string computerName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(buf, &n) ? utf8(std::wstring(buf, n)) : "?";
}

std::string windowsName() {
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string name = regString(HKEY_LOCAL_MACHINE, key, L"ProductName");
    const std::string build = regString(HKEY_LOCAL_MACHINE, key, L"CurrentBuildNumber");
    if (std::atoi(build.c_str()) >= 22000 && name.rfind("Windows 10", 0) == 0) name.replace(8, 2, "11");
    std::string ver = regString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
    return name + (ver.empty() ? "" : " " + ver);
}

std::string cpuName() {
    std::string s = regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    for (const char* junk : {"(R)", "(TM)", "(tm)", "CPU ", "Processor "}) {
        size_t p;
        while ((p = s.find(junk)) != std::string::npos) s.erase(p, std::strlen(junk));
    }
    s.erase(0, s.find_first_not_of(' '));
    return s.empty() ? "?" : s;
}

std::string memoryText() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (!GlobalMemoryStatusEx(&m)) return "?";
    return gb(static_cast<double>(m.ullTotalPhys - m.ullAvailPhys)) + " / " + gb(static_cast<double>(m.ullTotalPhys));
}

std::string uptimeText() {
    const unsigned long long s = GetTickCount64() / 1000;
    char b[48];
    std::snprintf(b, sizeof b, "%llud %lluh %llum", s / 86400, s / 3600 % 24, s / 60 % 60);
    return b;
}

std::string timeText() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[48];
    std::strftime(b, sizeof b, "%d %b %Y  %H:%M", &tm);
    return b;
}

std::string displayText() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return "?";
    return std::to_string(dm.dmPelsWidth) + " x " + std::to_string(dm.dmPelsHeight) + " @ " + std::to_string(dm.dmDisplayFrequency) + " Hz";
}

std::string scalingText() {
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return std::to_string(dpi * 100 / 96) + "%";
}

std::string monitorCount() { return std::to_string(GetSystemMetrics(SM_CMONITORS)); }

std::vector<std::pair<std::string, std::string>> drives() {
    std::vector<std::pair<std::string, std::string>> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER freeB{}, total{}, totalFree{};
        if (!GetDiskFreeSpaceExW(root, &freeB, &total, &totalFree)) continue;
        out.push_back({std::string(1, static_cast<char>('A' + i)) + ":",
                       gb(static_cast<double>(freeB.QuadPart)) + " free of " + gb(static_cast<double>(total.QuadPart))});
    }
    return out;
}

std::vector<Adapter> adapters() {
    std::vector<Adapter> out;
    ULONG size = 16384;
    std::vector<unsigned char> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstUnicastAddress) continue;
        char ip[64] = "";
        auto* sin = reinterpret_cast<sockaddr_in*>(a->FirstUnicastAddress->Address.lpSockaddr);
        InetNtopA(AF_INET, &sin->sin_addr, ip, sizeof ip);
        Adapter ad;
        ad.kind = a->IfType == IF_TYPE_IEEE80211 ? "WI-FI" : a->IfType == IF_TYPE_ETHERNET_CSMACD ? "ETHERNET" : "NETWORK";
        ad.ip = ip;
        const double mbps = static_cast<double>(a->TransmitLinkSpeed) / 1e6;
        char sp[32] = "";
        if (mbps > 0 && mbps < 1e6) std::snprintf(sp, sizeof sp, mbps >= 1000 ? "  %.0f Gbps" : "  %.0f Mbps", mbps >= 1000 ? mbps / 1000 : mbps);
        ad.detail = utf8(a->FriendlyName) + sp;
        out.push_back(std::move(ad));
    }
    return out;
}

}  // namespace sf::os
