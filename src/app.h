// Taskbar System Info - shared declarations
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#define APP_NAME  L"TaskbarSysInfo"
#define APP_TITLE L"Taskbar System Info"

constexpr UINT WM_APP_DATA = WM_APP + 1;  // worker -> UI: new sample available
constexpr UINT WM_APP_TRAY = WM_APP + 2;  // tray icon callback
constexpr UINT WM_APP_MENU = WM_APP + 3;  // widget right-click -> show menu
constexpr UINT WM_APP_UPDATE = WM_APP + 4;  // update check finished: wParam = manual, lParam = UpdateResult*
constexpr UINT WM_APP_INSTALLED = WM_APP + 5;  // self-update finished: lParam = UpdateResult*

enum Metric { M_CPU, M_GPU, M_RAM, M_VRAM, M_NET, M_COUNT };
enum Series { S_CPU, S_GPU, S_RAM, S_VRAM, S_DOWN, S_UP, S_COUNT };
constexpr Metric kSeriesMetric[S_COUNT] = { M_CPU, M_GPU, M_RAM, M_VRAM, M_NET, M_NET };
extern const wchar_t* const kMetricName[M_COUNT];

// ---------------------------------------------------------------- config
struct MetricCfg {
    bool show = true;
    bool graph = true;
    int historySec = 300;
    int graphWidth = 0;          // DIPs, 0 = use Config::graphWidth
    COLORREF color = 0;          // graph color (download color for network)
};

struct Config {
    MetricCfg m[M_COUNT];
    COLORREF netUpColor = RGB(0xFF, 0x8A, 0x65);
    int intervalMs = 1000;
    int taskbars = 0;            // 0 = main taskbar, 1 = second taskbar(s), 2 = all taskbars
    int position = 0;            // 0 = next to notification area, 1 = left edge of taskbar
    int offset = 0;              // extra distance from the anchor, in DIPs
    int graphWidth = 40;         // DIPs
    int spacing = 10;            // DIPs between items
    std::wstring fontName = L"Segoe UI";
    int fontSize = 9;            // points
    bool textAuto = true;        // follow taskbar light/dark theme
    COLORREF textColor = RGB(255, 255, 255);
    int bgAlpha = 0;             // 0 = fully transparent background
    bool clickThrough = false;   // widget ignores the mouse (taskbar icons below stay clickable)
    bool autoUpdate = true;      // check GitHub releases once a day
    long long lastUpdateCheck = 0;  // unix time of the last automatic check
    std::wstring updateNotified; // newest version we already showed a notification for
    COLORREF bgColor = RGB(32, 32, 32);
    std::wstring netInterface;   // interface alias, empty = all physical adapters
    std::wstring gpuAdapter;     // adapter description, empty = auto (largest dedicated VRAM)
    bool netBits = false;        // show bits/s instead of bytes/s
};

std::wstring ExeDir();
std::wstring ConfigPath();
void LoadConfig(Config& c);
void SaveConfig(const Config& c);

// ---------------------------------------------------------------- history ring buffer
class History {
public:
    void SetCapacity(size_t n) {
        if (n < 2) n = 2;
        if (n == buf_.size()) return;
        std::vector<float> nb(n, 0.f);
        size_t keep = std::min(count_, n);
        for (size_t i = 0; i < keep; ++i) nb[i] = At(count_ - keep + i);
        buf_.swap(nb);
        count_ = keep;
        head_ = keep % n;
    }
    void Push(float v) {
        if (buf_.empty()) return;
        buf_[head_] = v;
        head_ = (head_ + 1) % buf_.size();
        if (count_ < buf_.size()) ++count_;
    }
    size_t Capacity() const { return buf_.size(); }
    size_t Count() const { return count_; }
    float At(size_t i) const {  // 0 = oldest
        size_t start = (head_ + buf_.size() - count_) % buf_.size();
        return buf_[(start + i) % buf_.size()];
    }
private:
    std::vector<float> buf_;
    size_t head_ = 0, count_ = 0;
};

// ---------------------------------------------------------------- samples / shared state
struct Sample {
    double cpu = 0, gpu = 0;                 // percent
    uint64_t ramUsed = 0, ramTotal = 0;
    uint64_t vramUsed = 0, vramTotal = 0;
    double netDown = 0, netUp = 0;           // bytes per second
    double netTotalDown = 0, netTotalUp = 0; // bytes since app start (current interface selection)
    uint64_t commitUsed = 0, commitTotal = 0;
    bool gpuOk = false, vramOk = false;
    std::wstring gpuName, netName;
};

// Free-space anchors inside one Win11 taskbar, found via UI Automation.
struct TaskbarLayoutInfo {
    HWND taskbar = nullptr;
    RECT widgets{};               // Widgets/weather button (screen coords), empty if absent
    int trayLeft = INT32_MAX;     // left edge of tray icons / clock, INT32_MAX if unknown
};

struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;  // guards everything below
    Config cfg;
    Sample cur;
    History hist[S_COUNT];
    std::vector<TaskbarLayoutInfo> layout;
};
extern Shared g_shared;

// Taskbar windows selected by Config::taskbars (main = Shell_TrayWnd, others = Shell_SecondaryTrayWnd).
std::vector<HWND> FindTaskbars(int mode);

// ---------------------------------------------------------------- taskbar UI Automation probe (Win11)
class TaskbarProbe {
public:
    TaskbarProbe();
    ~TaskbarProbe();
    TaskbarProbe(const TaskbarProbe&) = delete;
    TaskbarProbe& operator=(const TaskbarProbe&) = delete;
    TaskbarLayoutInfo Probe(HWND taskbar);  // only meaningful for the XAML (Win11) taskbar
private:
    struct Impl;
    Impl* p_;
};
void ApplyHistoryCapacities();    // caller must hold g_shared.lock exclusively

// ---------------------------------------------------------------- metrics
struct NetIfInfo { std::wstring alias, desc; bool physical = false; };
struct GpuInfo { std::wstring name; LUID luid{}; uint64_t dedicated = 0; };
std::vector<NetIfInfo> EnumNetInterfaces();
std::vector<GpuInfo> EnumGpus();

class Sampler {
public:
    Sampler();
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    void Collect(Sample& s, const Config& c);
private:
    struct Impl;
    Impl* p_;
};

// ---------------------------------------------------------------- formatting
std::wstring FormatRate(double bytesPerSec, bool bits);
std::wstring FormatSize(uint64_t bytes);
std::wstring FormatPct(double pct);

// ---------------------------------------------------------------- taskbar widget
void Widget_Init(HINSTANCE inst, HWND owner);
void Widget_Update();       // re-attach if needed, layout, render, position
void Widget_Invalidate();   // force a new layout on next update
void Widget_Reattach();     // explorer restarted
void Widget_Shutdown();
bool LightTaskbarTheme();

// graph helpers shared by the widget and the flyout (UI thread only)
namespace Gdiplus { class Graphics; class RectF; }
void HistoryColumns(const History& h, int W, std::vector<float>& out);
void DrawGraphSeries(Gdiplus::Graphics& gr, const Gdiplus::RectF& r, const std::vector<float>& cols, float scale,
                     COLORREF color, bool fill, float penW);
float NiceRateScale(float bytesPerSec);

// ---------------------------------------------------------------- details flyout (click on widget)
void Flyout_Init(HINSTANCE inst, HWND owner);
void Flyout_Toggle(const RECT& anchor, HWND taskbar);  // anchor = clicked widget (screen coords)
void Flyout_Refresh();                                 // new sample arrived
void Flyout_Hide();
void Flyout_Shutdown();

// ---------------------------------------------------------------- update check (GitHub releases)
struct UpdateResult {
    bool ok = false;             // request + parse succeeded
    bool noReleases = false;     // repository has no published release yet
    bool newer = false;          // latest release is newer than this build
    std::wstring tag, url, error;
    std::wstring assetUrl, sha256;  // portable zip of the release and its checksum (empty if not found)
};
const wchar_t* AppVersion();
void Update_CheckAsync(HWND notify, bool manual);  // posts WM_APP_UPDATE when done
bool Update_InProgress();
// Downloads, verifies and swaps in the new exe; posts WM_APP_INSTALLED (lParam = UpdateResult*, ok/error/tag).
void Update_InstallAsync(HWND notify, const std::wstring& tag, const std::wstring& assetUrl, const std::wstring& sha256);
bool Update_Installing();
void Update_CleanupOld();  // deletes TaskbarSysInfo.exe.old left by a previous update
