// Portable configuration stored in TaskbarSysInfo.ini next to the executable.
#include "app.h"
#include <cwchar>
#include <cwctype>

const wchar_t* const kMetricName[M_COUNT] = { L"CPU", L"GPU", L"RAM", L"VRAM", L"Network" };

static const COLORREF kDefColor[M_COUNT] = {
    RGB(0x4F, 0xC3, 0xF7),  // CPU  - light blue
    RGB(0x81, 0xC7, 0x84),  // GPU  - green
    RGB(0xFF, 0xB7, 0x4D),  // RAM  - orange
    RGB(0xBA, 0x68, 0xC8),  // VRAM - purple
    RGB(0x4D, 0xD0, 0xE1),  // NET download - cyan
};

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    std::wstring s(buf, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? s : s.substr(0, k);
}

std::wstring ConfigPath() { return ExeDir() + L"\\" APP_NAME L".ini"; }

static std::wstring GetStr(const wchar_t* sec, const wchar_t* key, const std::wstring& def, const std::wstring& path) {
    wchar_t buf[512];
    GetPrivateProfileStringW(sec, key, def.c_str(), buf, ARRAYSIZE(buf), path.c_str());
    std::wstring s = buf;
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

static int GetInt(const wchar_t* sec, const wchar_t* key, int def, const std::wstring& path) {
    std::wstring s = GetStr(sec, key, L"", path);
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    return end == s.c_str() ? def : (int)v;
}

static COLORREF ParseColor(const std::wstring& s, COLORREF def) {
    const wchar_t* p = s.c_str();
    if (*p == L'#') ++p;
    if (wcslen(p) != 6) return def;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(p, &end, 16);
    if (*end) return def;
    return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

static std::wstring ColorStr(COLORREF c) {
    wchar_t b[8];
    swprintf_s(b, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

void LoadConfig(Config& c) {
    const std::wstring p = ConfigPath();
    const wchar_t* G = L"General";
    c.intervalMs = std::clamp(GetInt(G, L"IntervalMs", 1000, p), 250, 10000);
    c.taskbars = std::clamp(GetInt(G, L"Taskbars", 0, p), 0, 2);
    c.position = GetInt(G, L"Position", 0, p) ? 1 : 0;
    c.offset = std::clamp(GetInt(G, L"Offset", 0, p), -4000, 4000);
    c.graphWidth = std::clamp(GetInt(G, L"GraphWidth", 40, p), 10, 400);
    c.spacing = std::clamp(GetInt(G, L"Spacing", 10, p), 0, 200);
    c.fontName = GetStr(G, L"FontName", L"Segoe UI", p);
    if (c.fontName.empty()) c.fontName = L"Segoe UI";
    c.fontSize = std::clamp(GetInt(G, L"FontSize", 9, p), 6, 24);
    std::wstring tc = GetStr(G, L"TextColor", L"auto", p);
    c.textAuto = _wcsicmp(tc.c_str(), L"auto") == 0;
    c.textColor = ParseColor(tc, RGB(255, 255, 255));
    c.bgAlpha = std::clamp(GetInt(G, L"BackgroundAlpha", 0, p), 0, 255);
    c.clickThrough = GetInt(G, L"ClickThrough", 0, p) != 0;
    c.bgColor = ParseColor(GetStr(G, L"BackgroundColor", L"202020", p), RGB(32, 32, 32));

    for (int i = 0; i < M_COUNT; ++i) {
        MetricCfg& m = c.m[i];
        const wchar_t* s = kMetricName[i];
        m.show = GetInt(s, L"Show", 1, p) != 0;
        m.graph = GetInt(s, L"Graph", 1, p) != 0;
        m.historySec = std::clamp(GetInt(s, L"History", 300, p), 10, 3600);
        m.graphWidth = std::clamp(GetInt(s, L"GraphWidth", 0, p), 0, 400);
        m.color = ParseColor(GetStr(s, i == M_NET ? L"ColorDown" : L"Color", ColorStr(kDefColor[i]), p), kDefColor[i]);
    }
    c.netUpColor = ParseColor(GetStr(L"Network", L"ColorUp", L"FF8A65", p), RGB(0xFF, 0x8A, 0x65));
    c.netInterface = GetStr(L"Network", L"Interface", L"", p);
    c.netBits = GetInt(L"Network", L"Bits", 0, p) != 0;
    c.gpuAdapter = GetStr(L"GPU", L"Adapter", L"", p);
}

// WritePrivateProfileString keeps a file's encoding; create it as UTF-16 so that
// non-ASCII interface / adapter names survive.
static void EnsureUnicodeFile(const std::wstring& p) {
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const wchar_t header[] =
        L"\xFEFF; Taskbar System Info configuration.\r\n"
        L"; Most options can be changed from the tray icon menu. After editing this file by hand\r\n"
        L"; choose \"Reload configuration\" from the menu.\r\n"
        L"; Colors are RRGGBB hex. TextColor=auto follows the taskbar light/dark theme.\r\n"
        L"; Taskbars: 0 = main taskbar, 1 = second taskbar(s), 2 = all. Position: 0 = next to the notification area, 1 = left edge. Offset/GraphWidth/Spacing in pixels at 100% scale.\r\n"
        L"; History is in seconds. Network Interface: empty = all physical adapters, otherwise the adapter name.\r\n\r\n";
    DWORD written;
    WriteFile(h, header, (DWORD)(wcslen(header) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

void SaveConfig(const Config& c) {
    const std::wstring p = ConfigPath();
    EnsureUnicodeFile(p);
    auto put = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
        WritePrivateProfileStringW(sec, key, v.c_str(), p.c_str());
    };
    auto puti = [&](const wchar_t* sec, const wchar_t* key, int v) { put(sec, key, std::to_wstring(v)); };

    const wchar_t* G = L"General";
    puti(G, L"IntervalMs", c.intervalMs);
    puti(G, L"Taskbars", c.taskbars);
    puti(G, L"Position", c.position);
    puti(G, L"Offset", c.offset);
    puti(G, L"GraphWidth", c.graphWidth);
    puti(G, L"Spacing", c.spacing);
    put(G, L"FontName", c.fontName);
    puti(G, L"FontSize", c.fontSize);
    put(G, L"TextColor", c.textAuto ? L"auto" : ColorStr(c.textColor));
    puti(G, L"BackgroundAlpha", c.bgAlpha);
    puti(G, L"ClickThrough", c.clickThrough);
    put(G, L"BackgroundColor", ColorStr(c.bgColor));
    for (int i = 0; i < M_COUNT; ++i) {
        const MetricCfg& m = c.m[i];
        const wchar_t* s = kMetricName[i];
        puti(s, L"Show", m.show);
        puti(s, L"Graph", m.graph);
        puti(s, L"History", m.historySec);
        puti(s, L"GraphWidth", m.graphWidth);
        put(s, i == M_NET ? L"ColorDown" : L"Color", ColorStr(m.color));
    }
    put(L"Network", L"ColorUp", ColorStr(c.netUpColor));
    put(L"Network", L"Interface", c.netInterface);
    puti(L"Network", L"Bits", c.netBits);
    put(L"GPU", L"Adapter", c.gpuAdapter);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, p.c_str());  // flush cache
}

// ---------------------------------------------------------------- formatting helpers
std::wstring FormatRate(double bps, bool bits) {
    static const wchar_t* const kBytes[] = { L"B/s", L"KB/s", L"MB/s", L"GB/s" };
    static const wchar_t* const kBits[] = { L"b/s", L"Kb/s", L"Mb/s", L"Gb/s" };
    double v = bits ? bps * 8.0 : bps;
    const double base = bits ? 1000.0 : 1024.0;
    int u = 0;
    while (v >= 1000.0 && u < 3) { v /= base; ++u; }
    wchar_t b[32];
    if (u == 0 || v >= 100.0) swprintf_s(b, L"%.0f %s", v, bits ? kBits[u] : kBytes[u]);
    else swprintf_s(b, L"%.1f %s", v, bits ? kBits[u] : kBytes[u]);
    return b;
}

std::wstring FormatSize(uint64_t bytes) {
    wchar_t b[32];
    double gb = bytes / (1024.0 * 1024.0 * 1024.0);
    if (bytes < 1024ull * 1024) swprintf_s(b, L"%.0f KB", bytes / 1024.0);
    else if (gb < 1.0) swprintf_s(b, L"%.0f MB", bytes / (1024.0 * 1024.0));
    else if (gb < 100.0) swprintf_s(b, L"%.1f GB", gb);
    else swprintf_s(b, L"%.0f GB", gb);
    return b;
}

std::wstring FormatPct(double pct) {
    wchar_t b[16];
    swprintf_s(b, L"%.0f%%", std::clamp(pct, 0.0, 100.0));
    return b;
}
