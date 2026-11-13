// Details flyout shown when the taskbar widget (or the tray icon) is clicked, like the Win11 Widgets board:
// larger graphs with average / peak values, hardware names, top processes and quick actions.
// The window exists only while open; process sampling runs only while it is visible.
#include "app.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cmath>
#include <memory>
#include <unordered_map>

using namespace Gdiplus;

bool IsWin11Build();

namespace {

const wchar_t kClass[] = L"TaskbarSysInfoFlyout";
constexpr int kWidthDip = 380;
constexpr int kTopProcs = 5;

HINSTANCE s_hinst;
HWND s_owner, s_fly, s_taskbar;
RECT s_anchor;
ULONGLONG s_hiddenAt;
UINT s_dpi = 96;
float s_scale = 1.f;
bool s_light;
int s_graphH = 44;      // DIPs, reduced when the screen is short
bool s_showProcs = true;
int s_hotBtn = -1;
RECT s_btn[2];          // 0 = Task Manager, 1 = Settings
std::unique_ptr<Font> s_fHead, s_fValue, s_fText;

// static hardware info
std::wstring s_cpuName;
DWORD s_threads;

// ---------------------------------------------------------------- top processes (NtQuerySystemInformation)
struct SPI {  // SYSTEM_PROCESS_INFORMATION (stable layout since Vista)
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    USHORT NameLength;
    USHORT NameMaximumLength;
    PWSTR NameBuffer;
    LONG BasePriority;
    HANDLE UniqueProcessId;
};
using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

struct ProcRow { std::wstring name; double cpu; uint64_t mem; };
struct PrevTimes { ULONGLONG cpu; LONGLONG created; };
std::vector<ProcRow> s_procs;
std::unordered_map<ULONG_PTR, PrevTimes> s_prev, s_next;
ULONGLONG s_prevWall;
bool s_procsReady;
std::vector<BYTE> s_spiBuf;

void SampleProcesses() {
    static auto query = (NtQuerySystemInformationFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!query) return;
    if (s_spiBuf.empty()) s_spiBuf.resize(512 * 1024);
    LONG st = 0;
    for (int i = 0; i < 5; ++i) {
        ULONG need = 0;
        st = query(5 /*SystemProcessInformation*/, s_spiBuf.data(), (ULONG)s_spiBuf.size(), &need);
        if (st != (LONG)0xC0000004L /*STATUS_INFO_LENGTH_MISMATCH*/) break;
        s_spiBuf.resize(std::max<size_t>(size_t(need) + 64 * 1024, s_spiBuf.size() * 2));
    }
    if (st < 0) return;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const ULONGLONG now = (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const double wall = s_prevWall && now > s_prevWall ? double(now - s_prevWall) : 0.0;
    const double ncpu = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

    s_next.clear();
    std::vector<ProcRow> all;
    all.reserve(400);
    for (BYTE* p = s_spiBuf.data();;) {
        const SPI* e = reinterpret_cast<const SPI*>(p);
        const ULONG_PTR pid = (ULONG_PTR)e->UniqueProcessId;
        if (pid != 0) {  // skip System Idle Process
            const ULONGLONG t = ULONGLONG(e->UserTime.QuadPart + e->KernelTime.QuadPart);
            s_next[pid] = { t, e->CreateTime.QuadPart };
            double cpu = 0;
            auto it = s_prev.find(pid);
            if (wall > 0 && it != s_prev.end() && it->second.created == e->CreateTime.QuadPart && t >= it->second.cpu)
                cpu = double(t - it->second.cpu) / (wall * ncpu) * 100.0;
            std::wstring name = e->NameBuffer ? std::wstring(e->NameBuffer, e->NameLength / sizeof(wchar_t)) : L"System";
            all.push_back({ std::move(name), cpu, (uint64_t)std::max<LONGLONG>(0, e->WorkingSetPrivateSize.QuadPart) });
        }
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }
    s_prev.swap(s_next);
    s_procsReady = s_prevWall != 0;
    s_prevWall = now;

    const size_t n = std::min<size_t>(kTopProcs, all.size());
    std::partial_sort(all.begin(), all.begin() + n, all.end(), [](const ProcRow& a, const ProcRow& b) {
        return a.cpu != b.cpu ? a.cpu > b.cpu : a.mem > b.mem;
    });
    all.resize(n);
    s_procs.swap(all);
}

// ---------------------------------------------------------------- helpers
int S(float v) { return (int)std::lround(v * s_scale); }
Color ToColor(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

struct Palette {
    Color bg, text, sub, sep, graphBg, graphBorder, btn, btnHot;
};
Palette Colors() {
    if (s_light)
        return { Color(255, 249, 249, 249), Color(255, 26, 26, 26), Color(255, 96, 96, 96), Color(255, 229, 229, 229),
                 Color(10, 0, 0, 0), Color(28, 0, 0, 0), Color(255, 255, 255, 255), Color(255, 240, 240, 240) };
    return { Color(255, 36, 36, 36), Color(255, 255, 255, 255), Color(255, 168, 168, 168), Color(255, 58, 58, 58),
             Color(12, 255, 255, 255), Color(30, 255, 255, 255), Color(255, 50, 50, 50), Color(255, 62, 62, 62) };
}

void LoadStaticInfo() {
    if (!s_cpuName.empty()) return;
    wchar_t buf[256] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        s_cpuName = buf;
    while (!s_cpuName.empty() && s_cpuName.back() == L' ') s_cpuName.pop_back();
    while (!s_cpuName.empty() && s_cpuName.front() == L' ') s_cpuName.erase(0, 1);
    if (s_cpuName.empty()) s_cpuName = L"Processor";
    s_threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

void MakeFonts() {
    auto make = [](const wchar_t* fam, float px, int style) {
        auto f = std::make_unique<Font>(fam, px * s_scale, style, UnitPixel);
        if (f->GetLastStatus() != Ok) f = std::make_unique<Font>(L"Segoe UI", px * s_scale, style, UnitPixel);
        return f;
    };
    s_fHead = make(L"Segoe UI Semibold", 14.f, FontStyleRegular);
    s_fValue = make(L"Segoe UI Semibold", 16.f, FontStyleRegular);
    s_fText = make(L"Segoe UI", 12.f, FontStyleRegular);
}

std::wstring HistoryText(int sec) {
    if (sec < 60) return L"last " + std::to_wstring(sec) + L" s";
    if (sec < 3600) return L"last " + std::to_wstring(sec / 60) + L" min";
    return L"last hour";
}

std::wstring Uptime() {
    ULONGLONG m = GetTickCount64() / 60000;
    wchar_t b[64];
    if (m >= 1440) swprintf_s(b, L"Up %llu d %llu h", m / 1440, (m % 1440) / 60);
    else swprintf_s(b, L"Up %llu h %llu min", m / 60, m % 60);
    return b;
}

void Stats(const History& h, float& avg, float& peak) {
    avg = peak = 0;
    const size_t n = h.Count();
    if (!n) return;
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        float v = h.At(i);
        sum += v;
        peak = std::max(peak, v);
    }
    avg = float(sum / n);
}

// ---------------------------------------------------------------- layout + paint (one code path; draw=false only measures)
int DoLayout(Graphics& g, bool draw, const Config& c) {
    const Palette P = Colors();
    SolidBrush text(P.text), sub(P.sub);
    StringFormat left(StringFormat::GenericTypographic()), right(StringFormat::GenericTypographic());
    for (StringFormat* f : { &left, &right }) {
        f->SetFormatFlags(f->GetFormatFlags() | StringFormatFlagsNoWrap);
        f->SetTrimming(StringTrimmingEllipsisCharacter);
        f->SetLineAlignment(StringAlignmentCenter);
    }
    right.SetAlignment(StringAlignmentFar);

    const float W = (float)S(kWidthDip), pad = (float)S(16), inner = W - 2 * pad;
    const float headH = (float)S(24), lineH = (float)S(18), gh = (float)S((float)s_graphH), gap = (float)S(14);
    float y = pad - S(2);

    auto str = [&](const std::wstring& s, Font* f, float x, float yy, float w, float h, StringFormat& fmt, Brush& b) {
        if (draw) g.DrawString(s.c_str(), -1, f, RectF(x, yy, w, h), &fmt, &b);
    };
    auto separator = [&]() {
        if (draw) {
            Pen pen(P.sep, 1.f);
            g.DrawLine(&pen, 0.f, std::floor(y) + 0.5f, W, std::floor(y) + 0.5f);
        }
    };

    std::vector<int> metrics;
    for (int i = 0; i < M_COUNT; ++i) if (c.m[i].show) metrics.push_back(i);
    if (metrics.empty()) for (int i = 0; i < M_COUNT; ++i) metrics.push_back(i);

    AcquireSRWLockShared(&g_shared.lock);
    const Sample s = g_shared.cur;
    for (int m : metrics) {
        std::wstring value, detail, stats;
        const std::wstring when = HistoryText(c.m[m].historySec);
        float avg, peak, avg2, peak2;
        switch (m) {
        case M_CPU:
            value = FormatPct(s.cpu);
            detail = s_cpuName + L"  \x00B7  " + std::to_wstring(s_threads) + L" threads";
            Stats(g_shared.hist[S_CPU], avg, peak);
            stats = L"Avg " + FormatPct(avg) + L"  \x00B7  Peak " + FormatPct(peak) + L"  \x00B7  " + when;
            break;
        case M_GPU:
            value = s.gpuOk ? FormatPct(s.gpu) : L"n/a";
            detail = s.gpuName.empty() ? L"No GPU counters available" : s.gpuName;
            Stats(g_shared.hist[S_GPU], avg, peak);
            stats = L"Avg " + FormatPct(avg) + L"  \x00B7  Peak " + FormatPct(peak) + L"  \x00B7  " + when;
            break;
        case M_RAM:
            value = FormatPct(s.ramTotal ? 100.0 * s.ramUsed / s.ramTotal : 0);
            detail = FormatSize(s.ramUsed) + L" of " + FormatSize(s.ramTotal) + L" in use  \x00B7  Committed " +
                     FormatSize(s.commitUsed) + L" / " + FormatSize(s.commitTotal);
            Stats(g_shared.hist[S_RAM], avg, peak);
            stats = L"Avg " + FormatPct(avg) + L"  \x00B7  Peak " + FormatPct(peak) + L"  \x00B7  " + when;
            break;
        case M_VRAM:
            value = s.vramOk ? FormatPct(100.0 * s.vramUsed / s.vramTotal) : L"n/a";
            detail = s.vramOk ? FormatSize(s.vramUsed) + L" of " + FormatSize(s.vramTotal) + L" dedicated" +
                                    (s.gpuName.empty() ? L"" : L"  \x00B7  " + s.gpuName)
                              : L"Dedicated video memory not available";
            Stats(g_shared.hist[S_VRAM], avg, peak);
            stats = L"Avg " + FormatPct(avg) + L"  \x00B7  Peak " + FormatPct(peak) + L"  \x00B7  " + when;
            break;
        default:
            value = L"\x2193 " + FormatRate(s.netDown, c.netBits) + L"   \x2191 " + FormatRate(s.netUp, c.netBits);
            detail = (c.netInterface.empty() ? L"All physical adapters: " : L"Interface: ") + s.netName;
            Stats(g_shared.hist[S_DOWN], avg, peak);
            Stats(g_shared.hist[S_UP], avg2, peak2);
            stats = L"Peak \x2193 " + FormatRate(peak, c.netBits) + L" \x2191 " + FormatRate(peak2, c.netBits) +
                    L"  \x00B7  Session \x2193 " + FormatSize((uint64_t)s.netTotalDown) + L" \x2191 " +
                    FormatSize((uint64_t)s.netTotalUp);
            break;
        }

        // header: name + current value
        str(kMetricName[m], s_fHead.get(), pad, y, inner * 0.4f, headH, left, text);
        str(value, s_fValue.get(), pad + inner * 0.3f, y, inner * 0.7f, headH, right, text);
        y += headH;
        str(detail, s_fText.get(), pad, y, inner, lineH, left, sub);
        y += lineH + S(4);

        // graph
        if (draw) {
            RectF gr(pad, y, inner, gh);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            SolidBrush gb(P.graphBg);
            g.FillRectangle(&gb, gr);
            Pen gp(P.graphBorder, 1.f);
            g.DrawRectangle(&gp, gr.X, gr.Y, gr.Width - 1, gr.Height - 1);
            Pen grid(P.graphBorder, 1.f);
            grid.SetDashStyle(DashStyleDot);
            g.DrawLine(&grid, gr.X + 1, std::floor(gr.Y + gh / 2) + 0.5f, gr.X + gr.Width - 2, std::floor(gr.Y + gh / 2) + 0.5f);
            RectF r(gr.X + 1, gr.Y + 1, gr.Width - 2, gr.Height - 2);
            static std::vector<float> a, b;
            const float penW = std::max(1.f, 1.25f * s_scale);
            if (m == M_NET) {
                HistoryColumns(g_shared.hist[S_DOWN], (int)r.Width, a);
                HistoryColumns(g_shared.hist[S_UP], (int)r.Width, b);
                float mx = 0;
                for (float v : a) mx = std::max(mx, v);
                for (float v : b) mx = std::max(mx, v);
                const float scale = NiceRateScale(mx);
                DrawGraphSeries(g, r, a, scale, c.m[M_NET].color, true, penW);
                DrawGraphSeries(g, r, b, scale, c.netUpColor, false, penW);
                StringFormat tl(StringFormat::GenericTypographic());
                g.DrawString(FormatRate(scale, c.netBits).c_str(), -1, s_fText.get(), PointF(r.X + S(4), r.Y + S(2)), &tl, &sub);
            } else {
                static const Series kSer[] = { S_CPU, S_GPU, S_RAM, S_VRAM };
                HistoryColumns(g_shared.hist[kSer[m]], (int)r.Width, a);
                DrawGraphSeries(g, r, a, 100.f, c.m[m].color, true, penW);
            }
        }
        y += gh + S(4);
        str(stats, s_fText.get(), pad, y, inner, lineH, left, sub);
        y += lineH + gap;
    }
    ReleaseSRWLockShared(&g_shared.lock);

    // top processes
    if (s_showProcs) {
        separator();
        y += S(10);
        const float colMem = (float)S(70), colCpu = (float)S(56);
        str(L"Top processes", s_fHead.get(), pad, y, inner - colMem - colCpu, headH, left, text);
        str(L"CPU", s_fText.get(), pad + inner - colMem - colCpu, y, colCpu, headH, right, sub);
        str(L"Memory", s_fText.get(), pad + inner - colMem, y, colMem, headH, right, sub);
        y += headH;
        for (int i = 0; i < kTopProcs; ++i) {
            if (draw && i < (int)s_procs.size()) {
                const ProcRow& p = s_procs[i];
                wchar_t cpu[16];
                swprintf_s(cpu, p.cpu >= 10 ? L"%.0f%%" : L"%.1f%%", p.cpu);
                str(p.name, s_fText.get(), pad, y, inner - colMem - colCpu - S(8), lineH, left, text);
                str(s_procsReady ? cpu : L"\x2026", s_fText.get(), pad + inner - colMem - colCpu, y, colCpu, lineH, right, sub);
                str(FormatSize(p.mem), s_fText.get(), pad + inner - colMem, y, colMem, lineH, right, sub);
            }
            y += lineH;
        }
        y += S(10);
    }

    // footer: uptime + buttons
    separator();
    const float footH = (float)S(52), bh = (float)S(30);
    const float by = y + (footH - bh) / 2;
    str(Uptime(), s_fText.get(), pad, y, inner * 0.4f, footH, left, sub);
    const wchar_t* labels[2] = { L"Task Manager", L"Settings" };
    float bx = W - pad;
    for (int i = 1; i >= 0; --i) {
        RectF m;
        g.MeasureString(labels[i], -1, s_fText.get(), PointF(0, 0), StringFormat::GenericTypographic(), &m);
        const float bw = std::ceil(m.Width) + S(28);
        bx -= bw;
        s_btn[i] = { (LONG)bx, (LONG)by, (LONG)(bx + bw), (LONG)(by + bh) };
        if (draw) {
            GraphicsPath path;
            const float r = (float)S(4), d = r * 2;
            path.AddArc(bx, by, d, d, 180, 90);
            path.AddArc(bx + bw - d - 1, by, d, d, 270, 90);
            path.AddArc(bx + bw - d - 1, by + bh - d - 1, d, d, 0, 90);
            path.AddArc(bx, by + bh - d - 1, d, d, 90, 90);
            path.CloseFigure();
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            SolidBrush bb(s_hotBtn == i ? P.btnHot : P.btn);
            g.FillPath(&bb, &path);
            Pen bp(P.graphBorder, 1.f);
            g.DrawPath(&bp, &path);
            StringFormat cf(StringFormat::GenericTypographic());
            cf.SetAlignment(StringAlignmentCenter);
            cf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(labels[i], -1, s_fText.get(), RectF(bx, by, bw, bh), &cf, &text);
        }
        bx -= S(8);
    }
    y += footH;
    return (int)std::ceil(y);
}

void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(s_fly, &rc);
    const int w = rc.right, h = rc.bottom;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        const Config c = [] {
            AcquireSRWLockShared(&g_shared.lock);
            Config x = g_shared.cfg;
            ReleaseSRWLockShared(&g_shared.lock);
            return x;
        }();
        Graphics g(mem);
        g.Clear(Colors().bg);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        DoLayout(g, true, c);
        if (!IsWin11Build()) {  // Win10: no DWM rounded border, draw our own
            Pen p(Colors().sep, 1.f);
            g.DrawRectangle(&p, 0, 0, w - 1, h - 1);
        }
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

int HitButton(POINT pt) {
    for (int i = 0; i < 2; ++i)
        if (PtInRect(&s_btn[i], pt)) return i;
    return -1;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        Paint(hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) Flyout_Hide();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) Flyout_Hide();
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
        int hot = HitButton(pt);
        if (hot != s_hotBtn) {
            s_hotBtn = hot;
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
        }
        SetCursor(LoadCursorW(nullptr, hot >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s_hotBtn != -1) {
            s_hotBtn = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
        int b = HitButton(pt);
        if (b == 0) {
            Flyout_Hide();
            ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (b == 1) {
            Flyout_Hide();
            PostMessageW(s_owner, WM_APP_MENU, 0, 0);
        }
        return 0;
    }
    case WM_DPICHANGED:
        return 0;  // we size ourselves for the target monitor
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

// Windows 11 = build 22000+ (used for DWM rounded corners / border).
bool IsWin11Build() {
    static int cached = -1;
    if (cached < 0) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW vi{ sizeof(vi) };
        auto f = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        cached = f && f(&vi) == 0 && vi.dwBuildNumber >= 22000 ? 1 : 0;
    }
    return cached == 1;
}

void Flyout_Init(HINSTANCE inst, HWND owner) {
    s_hinst = inst;
    s_owner = owner;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

void Flyout_Toggle(const RECT& anchor, HWND taskbar) {
    if (s_fly && IsWindowVisible(s_fly)) {
        Flyout_Hide();
        return;
    }
    if (GetTickCount64() - s_hiddenAt < 300) return;  // this very click just closed it (focus loss)

    s_anchor = anchor;
    s_taskbar = taskbar;
    LoadStaticInfo();
    s_light = LightTaskbarTheme();
    s_hotBtn = -1;

    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : GetDpiForSystem();
    if (!dpi) dpi = 96;
    if (dpi != s_dpi || !s_fHead) {
        s_dpi = dpi;
        s_scale = dpi / 96.f;
        MakeFonts();
    }

    if (!s_fly) {
        s_fly = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, APP_TITLE, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                s_hinst, nullptr);
        if (!s_fly) return;
        const DWORD round = 2;  // DWMWCP_ROUND (Win11)
        DwmSetWindowAttribute(s_fly, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    }
    const COLORREF border = s_light ? RGB(220, 220, 220) : RGB(64, 64, 64);
    DwmSetWindowAttribute(s_fly, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));

    // processes: fresh baseline, rates appear on the next refresh
    s_prev.clear();
    s_prevWall = 0;
    s_procsReady = false;
    SampleProcesses();

    // size: shrink graphs / drop the process list on short screens
    const Config c = [] {
        AcquireSRWLockShared(&g_shared.lock);
        Config x = g_shared.cfg;
        ReleaseSRWLockShared(&g_shared.lock);
        return x;
    }();
    const int maxH = (mi.rcWork.bottom - mi.rcWork.top) - S(24);
    Bitmap tmp(1, 1, PixelFormat32bppARGB);
    Graphics mg(&tmp);
    s_showProcs = true;
    s_graphH = 44;
    int h = DoLayout(mg, false, c);
    while (h > maxH && s_graphH > 20) {
        s_graphH -= 4;
        h = DoLayout(mg, false, c);
    }
    if (h > maxH) {
        s_showProcs = false;
        h = DoLayout(mg, false, c);
    }
    const int w = S(kWidthDip);

    // position: above a bottom taskbar (below a top one, beside a side one), near the clicked widget
    const RECT& wk = mi.rcWork;
    const RECT& mr = mi.rcMonitor;
    const int m = S(12);
    int x = (anchor.left + anchor.right) / 2 - w / 2, y;
    if (wk.bottom < mr.bottom) y = wk.bottom - h - m;          // taskbar at bottom
    else if (wk.top > mr.top) y = wk.top + m;                  // top
    else y = wk.bottom - h - m;
    if (wk.left > mr.left) x = wk.left + m;                    // left
    else if (wk.right < mr.right) x = wk.right - w - m;        // right
    x = std::clamp(x, (int)wk.left + m, (int)wk.right - w - m);
    y = std::clamp(y, (int)wk.top + m, std::max((int)wk.top + m, (int)wk.bottom - h - m));

    SetWindowPos(s_fly, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(s_fly);
    InvalidateRect(s_fly, nullptr, FALSE);
}

void Flyout_Refresh() {
    if (!s_fly || !IsWindowVisible(s_fly)) return;
    SampleProcesses();
    InvalidateRect(s_fly, nullptr, FALSE);
}

void Flyout_Hide() {
    if (s_fly && IsWindowVisible(s_fly)) {
        ShowWindow(s_fly, SW_HIDE);
        s_hiddenAt = GetTickCount64();
    }
}

void Flyout_Shutdown() {
    if (s_fly) DestroyWindow(s_fly);
    s_fly = nullptr;
    s_fHead.reset();
    s_fValue.reset();
    s_fText.reset();
}
