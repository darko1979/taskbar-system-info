// Taskbar System Info - entry point, tray icon, menu and sampling thread.
#include "app.h"
#include <shellapi.h>
#include <objbase.h>
#include <objidl.h>
#include <gdiplus.h>
#include <ctime>
#include <memory>
#include "../res/resource.h"

Shared g_shared;

namespace {

HINSTANCE s_inst;
HWND s_main;
UINT s_wmTaskbarCreated;
HANDLE s_stopEvent, s_worker;
NOTIFYICONDATAW s_nid{};
bool s_cleanedUp;

const wchar_t kMainClass[] = L"TaskbarSysInfoMain";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

const int kHistoryOpts[] = { 30, 60, 120, 300, 600, 900, 1800, 3600 };
const int kIntervalOpts[] = { 500, 1000, 2000, 3000, 5000 };
const int kGraphWidthOpts[] = { 24, 32, 40, 50, 64, 80, 100 };
const int kFontOpts[] = { 8, 9, 10, 11, 12 };
const int kBgOpts[] = { 0, 80, 160, 255 };
const wchar_t* const kBgNames[] = { L"Transparent", L"Light tint", L"Medium", L"Opaque" };

enum : UINT {
    ID_METRIC = 1000,       // + metric * 100 + sub-id
    SUB_SHOW = 1, SUB_GRAPH = 2, SUB_HIST = 10, SUB_GW_DEFAULT = 30, SUB_GW = 31,
    ID_GPU_AUTO = 2000, ID_GPU_FIRST = 2001,
    ID_NET_ALL = 2100, ID_NET_FIRST = 2101,
    ID_NET_BYTES = 2190, ID_NET_BITS = 2191,
    ID_TASKBARS = 2700, ID_INTERVAL = 2200, ID_POS = 2300, ID_GW = 2400, ID_FONT = 2500, ID_BG = 2600,
    ID_AUTOSTART = 2900, ID_EDITCFG, ID_RELOAD, ID_TASKMGR, ID_EXIT,
    ID_CLICKTHROUGH = 2910,
    ID_UPDATE_CHECK = 2920, ID_UPDATE_AUTO, ID_UPDATE_DOWNLOAD, ID_UPDATE_PAGE,
};

// Update checks: first one shortly after start, then the timer re-checks hourly whether a day has passed.
constexpr UINT_PTR kUpdateTimer = 1;
constexpr UINT kUpdateFirstDelayMs = 30 * 1000;
constexpr UINT kUpdateTimerMs = 60 * 60 * 1000;
constexpr long long kUpdateEverySec = 24 * 60 * 60;
std::wstring s_updateTag, s_updateUrl;  // set when a newer release was found (url = release page)
std::wstring s_updateAsset, s_updateSha;  // its portable zip and SHA-256, for the self-update
std::wstring s_updatedFrom;  // set at start-up after a self-update (previous version)
HANDLE s_mutex = nullptr;    // single-instance mutex

// ---------------------------------------------------------------- sampling thread
DWORD WINAPI WorkerProc(void*) {
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
    Sampler sampler;
    TaskbarProbe probe;
    ULONGLONG lastProbe = 0;
    for (;;) {
        // Every 2 s: free-space anchors (Widgets/weather button, tray/clock) of each Win11 taskbar in use.
        // The weather button's width changes with the forecast text.
        const ULONGLONG now = GetTickCount64();
        if (now - lastProbe >= 2000) {
            lastProbe = now;
            AcquireSRWLockShared(&g_shared.lock);
            const int mode = g_shared.cfg.taskbars;
            ReleaseSRWLockShared(&g_shared.lock);
            std::vector<TaskbarLayoutInfo> layout;
            for (HWND tb : FindTaskbars(mode)) layout.push_back(probe.Probe(tb));
            AcquireSRWLockExclusive(&g_shared.lock);
            g_shared.layout.swap(layout);
            ReleaseSRWLockExclusive(&g_shared.lock);
        }

        Config c;
        AcquireSRWLockShared(&g_shared.lock);
        c = g_shared.cfg;
        ReleaseSRWLockShared(&g_shared.lock);

        Sample s;
        sampler.Collect(s, c);

        AcquireSRWLockExclusive(&g_shared.lock);
        g_shared.cur = s;
        g_shared.hist[S_CPU].Push((float)s.cpu);
        g_shared.hist[S_GPU].Push((float)s.gpu);
        g_shared.hist[S_RAM].Push(s.ramTotal ? float(100.0 * s.ramUsed / s.ramTotal) : 0.f);
        g_shared.hist[S_VRAM].Push(s.vramTotal ? float(100.0 * s.vramUsed / s.vramTotal) : 0.f);
        g_shared.hist[S_DOWN].Push((float)s.netDown);
        g_shared.hist[S_UP].Push((float)s.netUp);
        ReleaseSRWLockExclusive(&g_shared.lock);

        PostMessageW(s_main, WM_APP_DATA, 0, 0);
        if (WaitForSingleObject(s_stopEvent, c.intervalMs) == WAIT_OBJECT_0) break;
    }
    }
    if (SUCCEEDED(hrCom)) CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- autostart (HKCU Run)
std::wstring ExePathQuoted() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return L"\"" + std::wstring(buf, n) + L"\"";
}

bool IsAutostart() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    if (on) {
        std::wstring v = ExePathQuoted();
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME);
    }
}

// ---------------------------------------------------------------- tray icon
void AddTrayIcon() {
    s_nid = {};
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = s_main;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    s_nid.uCallbackMessage = WM_APP_TRAY;
    s_nid.hIcon = (HICON)LoadImageW(s_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wcscpy_s(s_nid.szTip, APP_TITLE);
    Shell_NotifyIconW(NIM_ADD, &s_nid);
    s_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &s_nid);
}

void UpdateTrayTip() {
    Sample s;
    Config c;
    AcquireSRWLockShared(&g_shared.lock);
    s = g_shared.cur;
    c = g_shared.cfg;
    ReleaseSRWLockShared(&g_shared.lock);

    std::wstring t = L"CPU " + FormatPct(s.cpu);
    if (s.gpuOk) t += L"   GPU " + FormatPct(s.gpu);
    t += L"\nRAM " + FormatSize(s.ramUsed) + L" / " + FormatSize(s.ramTotal);
    if (s.vramOk) t += L"\nVRAM " + FormatSize(s.vramUsed) + L" / " + FormatSize(s.vramTotal);
    t += L"\n\x2191 " + FormatRate(s.netUp, c.netBits) + L"   \x2193 " + FormatRate(s.netDown, c.netBits);
    if (wcscmp(t.c_str(), s_nid.szTip) == 0) return;
    wcsncpy_s(s_nid.szTip, t.c_str(), _TRUNCATE);
    s_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &s_nid);
}

// ---------------------------------------------------------------- dark context menus (undocumented, optional)
void EnableDarkMenus(HWND hwnd) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using AllowDarkForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
    using FlushMenuThemesFn = void(WINAPI*)();
    if (auto f = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135))) f(1 /*AllowDark*/);
    if (auto f = (AllowDarkForWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133))) f(hwnd, TRUE);
    if (auto f = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136))) f();
}

// ---------------------------------------------------------------- menu
void Append(HMENU m, UINT id, const std::wstring& text, bool checked = false, bool radio = false, bool enabled = true) {
    MENUITEMINFOW mi{ sizeof(mi) };
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    mi.fType = MFT_STRING | (radio ? MFT_RADIOCHECK : 0);
    mi.fState = (checked ? MFS_CHECKED : 0) | (enabled ? 0 : MFS_DISABLED);
    mi.wID = id;
    mi.dwTypeData = const_cast<wchar_t*>(text.c_str());
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}

void AppendSub(HMENU m, HMENU sub, const std::wstring& text, bool checked = false) {
    MENUITEMINFOW mi{ sizeof(mi) };
    mi.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_STATE;
    mi.fState = checked ? MFS_CHECKED : 0;
    mi.hSubMenu = sub;
    mi.dwTypeData = const_cast<wchar_t*>(text.c_str());
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}

void Sep(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }

std::wstring HistoryLabel(int sec) {
    if (sec < 60) return std::to_wstring(sec) + L" seconds";
    if (sec == 60) return L"1 minute";
    if (sec < 3600) return std::to_wstring(sec / 60) + L" minutes";
    return L"1 hour";
}

// ---------------------------------------------------------------- self-update
void ShowBalloon(const std::wstring& title, const std::wstring& text);
void Cleanup();

void OpenReleasePage() {
    if (!s_updateUrl.empty()) ShellExecuteW(nullptr, L"open", s_updateUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Downloads and installs s_updateTag, then restarts. Falls back to the release page if the release
// has no verifiable portable zip.
void StartInstall(bool confirm) {
    if (s_updateTag.empty() || Update_Installing()) return;
    const UINT mb = MB_SETFOREGROUND | MB_TOPMOST;
    if (s_updateAsset.empty() || s_updateSha.empty()) {
        if (MessageBoxW(s_main, (L"Version " + s_updateTag + L" cannot be installed automatically.\n\nOpen the download page?").c_str(),
                        APP_TITLE, mb | MB_ICONINFORMATION | MB_YESNO) == IDYES)
            OpenReleasePage();
        return;
    }
    if (confirm) {
        const std::wstring t = L"A new version is available: " + s_updateTag + L"\nYou are running version " +
                               AppVersion() +
                               L".\n\nInstall it now? The update is downloaded from GitHub, verified, and the app "
                               L"restarts. Your settings are kept.\n\nChoose Cancel to open the release page instead.";
        const int r = MessageBoxW(s_main, t.c_str(), APP_TITLE, mb | MB_ICONINFORMATION | MB_YESNOCANCEL);
        if (r == IDCANCEL) OpenReleasePage();
        if (r != IDYES) return;
    }
    Update_InstallAsync(s_main, s_updateTag, s_updateAsset, s_updateSha);
    ShowBalloon(L"Updating to " + s_updateTag, L"Downloading the update. The app restarts when it is installed.");
}

void OnInstalled(UpdateResult* res) {
    std::unique_ptr<UpdateResult> r(res);
    const UINT mb = MB_SETFOREGROUND | MB_TOPMOST;
    if (!r->ok) {
        if (MessageBoxW(s_main, (L"The update to " + r->tag + L" failed.\n\n" + r->error + L"\n\nOpen the download page?").c_str(),
                        APP_TITLE, mb | MB_ICONWARNING | MB_YESNO) == IDYES)
            OpenReleasePage();
        return;
    }
    // The new exe is in place: remove our widgets and tray icon, release the single-instance mutex,
    // start the new exe (it also waits for this process to exit) and quit.
    Cleanup();
    if (s_mutex) {
        CloseHandle(s_mutex);
        s_mutex = nullptr;
    }
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" /updated " + std::to_wstring(GetCurrentProcessId()) + L" " + AppVersion();
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        MessageBoxW(s_main, (L"Version " + r->tag + L" was installed, but the app could not be restarted.\n\nPlease start "
                             L"TaskbarSysInfo.exe again.").c_str(), APP_TITLE, mb | MB_ICONWARNING);
    } else {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DestroyWindow(s_main);
}

void ApplyCommand(UINT id, const std::vector<GpuInfo>& gpus, const std::vector<NetIfInfo>& nets) {
    switch (id) {
    case 0: return;
    case ID_EXIT: DestroyWindow(s_main); return;
    case ID_AUTOSTART: SetAutostart(!IsAutostart()); return;
    case ID_TASKMGR: ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL); return;
    case ID_EDITCFG: {
        std::wstring arg = L"\"" + ConfigPath() + L"\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    case ID_UPDATE_CHECK:
        Update_CheckAsync(s_main, true);
        return;
    case ID_UPDATE_DOWNLOAD:
        StartInstall(false);
        return;
    case ID_UPDATE_PAGE:
        OpenReleasePage();
        return;
    }

    AcquireSRWLockExclusive(&g_shared.lock);
    Config& c = g_shared.cfg;
    if (id == ID_RELOAD) {
        LoadConfig(c);
    } else if (id >= ID_METRIC && id < ID_METRIC + M_COUNT * 100) {
        MetricCfg& m = c.m[(id - ID_METRIC) / 100];
        UINT sub = (id - ID_METRIC) % 100;
        if (sub == SUB_SHOW) m.show = !m.show;
        else if (sub == SUB_GRAPH) m.graph = !m.graph;
        else if (sub >= SUB_HIST && sub < SUB_HIST + ARRAYSIZE(kHistoryOpts)) m.historySec = kHistoryOpts[sub - SUB_HIST];
        else if (sub == SUB_GW_DEFAULT) m.graphWidth = 0;
        else if (sub >= SUB_GW && sub < SUB_GW + ARRAYSIZE(kGraphWidthOpts)) m.graphWidth = kGraphWidthOpts[sub - SUB_GW];
    } else if (id == ID_GPU_AUTO) {
        c.gpuAdapter.clear();
    } else if (id >= ID_GPU_FIRST && id < ID_GPU_FIRST + gpus.size()) {
        c.gpuAdapter = gpus[id - ID_GPU_FIRST].name;
    } else if (id == ID_NET_ALL) {
        c.netInterface.clear();
    } else if (id >= ID_NET_FIRST && id < ID_NET_FIRST + nets.size()) {
        c.netInterface = nets[id - ID_NET_FIRST].alias;
    } else if (id == ID_NET_BYTES || id == ID_NET_BITS) {
        c.netBits = id == ID_NET_BITS;
    } else if (id >= ID_INTERVAL && id < ID_INTERVAL + ARRAYSIZE(kIntervalOpts)) {
        c.intervalMs = kIntervalOpts[id - ID_INTERVAL];
    } else if (id >= ID_TASKBARS && id < ID_TASKBARS + 3) {
        c.taskbars = id - ID_TASKBARS;
    } else if (id >= ID_POS && id < ID_POS + 2) {
        c.position = id - ID_POS;
    } else if (id >= ID_GW && id < ID_GW + ARRAYSIZE(kGraphWidthOpts)) {
        c.graphWidth = kGraphWidthOpts[id - ID_GW];
    } else if (id >= ID_FONT && id < ID_FONT + ARRAYSIZE(kFontOpts)) {
        c.fontSize = kFontOpts[id - ID_FONT];
    } else if (id >= ID_BG && id < ID_BG + ARRAYSIZE(kBgOpts)) {
        c.bgAlpha = kBgOpts[id - ID_BG];
    } else if (id == ID_CLICKTHROUGH) {
        c.clickThrough = !c.clickThrough;
    } else if (id == ID_UPDATE_AUTO) {
        c.autoUpdate = !c.autoUpdate;
        if (c.autoUpdate) SetTimer(s_main, kUpdateTimer, kUpdateFirstDelayMs, nullptr);  // check soon if overdue
    }
    ApplyHistoryCapacities();
    const Config copy = c;
    ReleaseSRWLockExclusive(&g_shared.lock);

    if (id != ID_RELOAD) SaveConfig(copy);
    Widget_Invalidate();
    Widget_Update();
}

// Clicking (left or right) a metric's own item ("CPU", "GPU", ...) toggles its "Show" setting,
// instead of only opening the submenu. Menus don't report clicks on submenu items, so a
// message-filter hook watches mouse-up events inside the menu's modal loop.
HMENU s_menuTop = nullptr;
HMENU s_metricSub[M_COUNT] = {};
int s_menuToggle = -1;

LRESULT CALLBACK MenuFilterProc(int code, WPARAM wp, LPARAM lp) {
    const MSG* msg = reinterpret_cast<const MSG*>(lp);
    if (code == MSGF_MENU && msg && (msg->message == WM_LBUTTONUP || msg->message == WM_RBUTTONUP)) {
        POINT pt = msg->pt;
        HWND w = WindowFromPoint(pt);
        wchar_t cls[16] = {};
        if (w && GetClassNameW(w, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"#32768") == 0 &&
            (HMENU)SendMessageW(w, MN_GETHMENU, 0, 0) == s_menuTop) {
            int pos = MenuItemFromPoint(nullptr, s_menuTop, pt);
            HMENU sub = pos >= 0 ? GetSubMenu(s_menuTop, pos) : nullptr;
            for (int i = 0; sub && i < M_COUNT; ++i) {
                if (s_metricSub[i] == sub) {
                    s_menuToggle = i;
                    EndMenu();
                    return 1;  // swallow the click
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void ShowMenu() {
    Config c;
    AcquireSRWLockShared(&g_shared.lock);
    c = g_shared.cfg;
    ReleaseSRWLockShared(&g_shared.lock);
    const std::vector<GpuInfo> gpus = EnumGpus();
    const std::vector<NetIfInfo> nets = EnumNetInterfaces();

    HMENU menu = CreatePopupMenu();
    if (!s_updateTag.empty()) {
        if (Update_Installing()) {
            Append(menu, 0, L"\x2B07  Downloading update " + s_updateTag + L"...", false, false, false);
        } else {
            Append(menu, ID_UPDATE_DOWNLOAD, L"\x2B07  Install update " + s_updateTag + L" and restart");
            SetMenuDefaultItem(menu, ID_UPDATE_DOWNLOAD, FALSE);  // bold
        }
        Append(menu, ID_UPDATE_PAGE, L"What's new in " + s_updateTag + L"...");
        Sep(menu);
    }
    for (int i = 0; i < M_COUNT; ++i) {
        const MetricCfg& m = c.m[i];
        const UINT base = ID_METRIC + i * 100;
        HMENU sub = CreatePopupMenu();
        Append(sub, base + SUB_SHOW, L"Show", m.show);
        Append(sub, base + SUB_GRAPH, L"Show history graph", m.graph, false, m.show);
        HMENU hist = CreatePopupMenu();
        for (UINT k = 0; k < ARRAYSIZE(kHistoryOpts); ++k)
            Append(hist, base + SUB_HIST + k, HistoryLabel(kHistoryOpts[k]), m.historySec == kHistoryOpts[k], true);
        AppendSub(sub, hist, L"History length");
        HMENU mgw = CreatePopupMenu();
        Append(mgw, base + SUB_GW_DEFAULT, L"Default (" + std::to_wstring(c.graphWidth) + L" px)", m.graphWidth == 0, true);
        Sep(mgw);
        for (UINT k = 0; k < ARRAYSIZE(kGraphWidthOpts); ++k)
            Append(mgw, base + SUB_GW + k, std::to_wstring(kGraphWidthOpts[k]) + L" px", m.graphWidth == kGraphWidthOpts[k], true);
        AppendSub(sub, mgw, L"Graph width");

        if (i == M_GPU || i == M_VRAM) {
            HMENU ad = CreatePopupMenu();
            Append(ad, ID_GPU_AUTO, L"Automatic (largest dedicated memory)", c.gpuAdapter.empty(), true);
            if (!gpus.empty()) Sep(ad);
            for (UINT k = 0; k < gpus.size() && k < 90; ++k)
                Append(ad, ID_GPU_FIRST + k, gpus[k].name + L"  (" + FormatSize(gpus[k].dedicated) + L")",
                       _wcsicmp(c.gpuAdapter.c_str(), gpus[k].name.c_str()) == 0, true);
            Sep(sub);
            AppendSub(sub, ad, L"Graphics adapter");
        }
        if (i == M_NET) {
            HMENU ifm = CreatePopupMenu();
            Append(ifm, ID_NET_ALL, L"All physical adapters", c.netInterface.empty(), true);
            if (!nets.empty()) Sep(ifm);
            bool found = c.netInterface.empty();
            for (UINT k = 0; k < nets.size() && k < 80; ++k) {
                bool sel = _wcsicmp(c.netInterface.c_str(), nets[k].alias.c_str()) == 0;
                found |= sel;
                Append(ifm, ID_NET_FIRST + k, nets[k].alias + L"  \x2014  " + nets[k].desc, sel, true);
            }
            if (!found) Append(ifm, 0, c.netInterface + L"  (not connected)", true, true, false);
            Sep(sub);
            AppendSub(sub, ifm, L"Interface");
            HMENU un = CreatePopupMenu();
            Append(un, ID_NET_BYTES, L"Bytes per second (KB/s, MB/s)", !c.netBits, true);
            Append(un, ID_NET_BITS, L"Bits per second (Kb/s, Mb/s)", c.netBits, true);
            AppendSub(sub, un, L"Units");
        }
        AppendSub(menu, sub, kMetricName[i], m.show);
        s_metricSub[i] = sub;
    }
    Sep(menu);

    HMENU iv = CreatePopupMenu();
    for (UINT k = 0; k < ARRAYSIZE(kIntervalOpts); ++k) {
        wchar_t b[32];
        swprintf_s(b, L"%g s", kIntervalOpts[k] / 1000.0);
        Append(iv, ID_INTERVAL + k, b, c.intervalMs == kIntervalOpts[k], true);
    }
    AppendSub(menu, iv, L"Update interval");

    HMENU tbm = CreatePopupMenu();
    const bool multi = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr) != nullptr;
    Append(tbm, ID_TASKBARS + 0, L"Main taskbar", c.taskbars == 0, true);
    Append(tbm, ID_TASKBARS + 1, multi ? L"Second taskbar" : L"Second taskbar (no second monitor)", c.taskbars == 1, true);
    Append(tbm, ID_TASKBARS + 2, L"All taskbars", c.taskbars == 2, true);
    AppendSub(menu, tbm, L"Show on");

    HMENU pos = CreatePopupMenu();
    Append(pos, ID_POS + 0, L"Next to notification area", c.position == 0, true);
    Append(pos, ID_POS + 1, L"Left side of taskbar", c.position == 1, true);
    AppendSub(menu, pos, L"Position");

    HMENU gw = CreatePopupMenu();
    for (UINT k = 0; k < ARRAYSIZE(kGraphWidthOpts); ++k)
        Append(gw, ID_GW + k, std::to_wstring(kGraphWidthOpts[k]) + L" px", c.graphWidth == kGraphWidthOpts[k], true);
    AppendSub(menu, gw, L"Default graph width");

    HMENU fs = CreatePopupMenu();
    for (UINT k = 0; k < ARRAYSIZE(kFontOpts); ++k)
        Append(fs, ID_FONT + k, std::to_wstring(kFontOpts[k]) + L" pt", c.fontSize == kFontOpts[k], true);
    AppendSub(menu, fs, L"Font size");

    HMENU bg = CreatePopupMenu();
    for (UINT k = 0; k < ARRAYSIZE(kBgOpts); ++k) Append(bg, ID_BG + k, kBgNames[k], c.bgAlpha == kBgOpts[k], true);
    AppendSub(menu, bg, L"Background");

    Sep(menu);
    Append(menu, ID_CLICKTHROUGH, L"Click-through widget (taskbar icons below stay clickable)", c.clickThrough);
    Append(menu, ID_AUTOSTART, L"Start with Windows", IsAutostart());
    Append(menu, ID_TASKMGR, L"Open Task Manager");
    Append(menu, ID_EDITCFG, L"Edit configuration file...");
    Append(menu, ID_RELOAD, L"Reload configuration");
    Sep(menu);
    Append(menu, ID_UPDATE_CHECK, Update_InProgress() ? L"Checking for updates..." : L"Check for updates now", false, false,
           !Update_InProgress());
    Append(menu, ID_UPDATE_AUTO, L"Check for updates automatically", c.autoUpdate);
    Append(menu, 0, std::wstring(APP_TITLE L" ") + AppVersion(), false, false, false);
    Sep(menu);
    Append(menu, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(s_main);
    s_menuTop = menu;
    s_menuToggle = -1;
    HHOOK hook = SetWindowsHookExW(WH_MSGFILTER, MenuFilterProc, nullptr, GetCurrentThreadId());
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, s_main, nullptr);
    if (hook) UnhookWindowsHookEx(hook);
    PostMessageW(s_main, WM_NULL, 0, 0);
    DestroyMenu(menu);
    s_menuTop = nullptr;
    if (cmd == 0 && s_menuToggle >= 0) cmd = ID_METRIC + s_menuToggle * 100 + SUB_SHOW;
    ApplyCommand(cmd, gpus, nets);
}

// ---------------------------------------------------------------- update check results
void ShowBalloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW n = s_nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void MaybeAutoCheck() {
    AcquireSRWLockShared(&g_shared.lock);
    const bool on = g_shared.cfg.autoUpdate;
    const long long last = g_shared.cfg.lastUpdateCheck;
    ReleaseSRWLockShared(&g_shared.lock);
    const long long now = _time64(nullptr);
    if (on && (now - last >= kUpdateEverySec || last > now)) Update_CheckAsync(s_main, false);
}

void OnUpdateResult(bool manual, UpdateResult* res) {
    std::unique_ptr<UpdateResult> r(res);
    if (r->newer) {
        s_updateTag = r->tag;
        s_updateUrl = r->url;
        s_updateAsset = r->assetUrl;
        s_updateSha = r->sha256;
    } else if (r->ok) {
        s_updateTag.clear();
        s_updateUrl.clear();
        s_updateAsset.clear();
        s_updateSha.clear();
    }

    // remember the check (and which version we announced) in the INI
    bool notify = false;
    if (r->ok) {
        AcquireSRWLockExclusive(&g_shared.lock);
        g_shared.cfg.lastUpdateCheck = _time64(nullptr);
        if (!manual && r->newer && _wcsicmp(g_shared.cfg.updateNotified.c_str(), r->tag.c_str()) != 0) {
            g_shared.cfg.updateNotified = r->tag;
            notify = true;
        }
        const Config copy = g_shared.cfg;
        ReleaseSRWLockExclusive(&g_shared.lock);
        SaveConfig(copy);
    }

    const std::wstring cur = AppVersion();
    if (!manual) {  // automatic: stay quiet unless there is something new (once per version)
        if (notify)
            ShowBalloon(L"Update available: " + r->tag,
                        L"Taskbar System Info " + r->tag + L" is available (you have " + cur + L"). Click to install it.");
        return;
    }
    const UINT mb = MB_SETFOREGROUND | MB_TOPMOST;
    if (!r->ok) {
        MessageBoxW(s_main, (L"Could not check for updates.\n\n" + r->error).c_str(), APP_TITLE, mb | MB_ICONWARNING);
    } else if (r->noReleases) {
        MessageBoxW(s_main, (L"No releases have been published yet.\n\nYou are running version " + cur + L".").c_str(),
                    APP_TITLE, mb | MB_ICONINFORMATION);
    } else if (r->newer) {
        StartInstall(true);
    } else {
        MessageBoxW(s_main, (L"You are running the latest version (" + cur + L").").c_str(), APP_TITLE,
                    mb | MB_ICONINFORMATION);
    }
}

// ---------------------------------------------------------------- lifetime
void Cleanup() {
    if (s_cleanedUp) return;
    s_cleanedUp = true;
    if (s_worker) {
        SetEvent(s_stopEvent);
        WaitForSingleObject(s_worker, 5000);
        CloseHandle(s_worker);
        s_worker = nullptr;
    }
    Flyout_Shutdown();
    Widget_Shutdown();
    Shell_NotifyIconW(NIM_DELETE, &s_nid);
    if (s_nid.hIcon) DestroyIcon(s_nid.hIcon);
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_DATA:
        if (!s_cleanedUp) {
            Widget_Update();
            UpdateTrayTip();
            Flyout_Refresh();
        }
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
            Flyout_Hide();
            ShowMenu();
            break;
        case NIN_SELECT:
        case NIN_KEYSELECT: {  // left click: details flyout above the tray icon
            const POINT pt{ (short)LOWORD(wp), (short)HIWORD(wp) };
            const RECT anchor{ pt.x - 1, pt.y - 1, pt.x + 1, pt.y + 1 };
            Flyout_Toggle(anchor, FindWindowW(L"Shell_TrayWnd", nullptr));
            break;
        }
        case NIN_BALLOONUSERCLICK:  // "update available" notification clicked
            StartInstall(true);
            break;
        }
        return 0;
    case WM_APP_INSTALLED:
        OnInstalled(reinterpret_cast<UpdateResult*>(lp));
        return 0;
    case WM_APP_MENU:
        ShowMenu();
        return 0;
    case WM_APP_UPDATE:
        OnUpdateResult(wp != 0, reinterpret_cast<UpdateResult*>(lp));
        return 0;
    case WM_TIMER:
        if (wp == kUpdateTimer) {
            SetTimer(h, kUpdateTimer, kUpdateTimerMs, nullptr);  // after the first shot: hourly
            MaybeAutoCheck();
        }
        return 0;
    case WM_COMMAND:  // menu commands can also be sent directly (e.g. for scripting/tests)
        if (HIWORD(wp) == 0 && LOWORD(wp) >= ID_AUTOSTART) ApplyCommand(LOWORD(wp), {}, {});
        return 0;
    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_THEMECHANGED:
        Widget_Invalidate();
        return DefWindowProcW(h, msg, wp, lp);
    case WM_ENDSESSION:
        if (wp) Cleanup();
        return 0;
    case WM_DESTROY:
        Cleanup();
        PostQuitMessage(0);
        return 0;
    }
    if (msg == s_wmTaskbarCreated && s_wmTaskbarCreated) {  // explorer (re)started
        AddTrayIcon();
        Widget_Reattach();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void UseEfficiencyMode() {
    // EcoQoS: lets Windows run this process on efficient cores / low clocks (Win10 1709+, ignored elsewhere).
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st));
}

}  // namespace

void ApplyHistoryCapacities() {
    const Config& c = g_shared.cfg;
    for (int i = 0; i < S_COUNT; ++i) {
        size_t n = size_t(c.m[kSeriesMetric[i]].historySec) * 1000 / size_t(c.intervalMs);
        g_shared.hist[i].SetCapacity(std::max<size_t>(n, 2));
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // "/updated <pid> <old version>": started by a self-update; wait for the old instance to exit first.
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc >= 3 && _wcsicmp(argv[1], L"/updated") == 0) {
            if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)wcstoul(argv[2], nullptr, 10))) {
                WaitForSingleObject(old, 15000);
                CloseHandle(old);
            }
            s_updatedFrom = argc >= 4 ? argv[3] : L"?";
        }
        LocalFree(argv);
    }
    Update_CleanupOld();

    s_mutex = CreateMutexW(nullptr, TRUE, L"Local\\TaskbarSysInfo.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    s_inst = inst;
    UseEfficiencyMode();

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);

    const bool cfgExisted = GetFileAttributesW(ConfigPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    LoadConfig(g_shared.cfg);
    ApplyHistoryCapacities();
    if (!cfgExisted) SaveConfig(g_shared.cfg);
    if (IsAutostart()) SetAutostart(true);  // portable: keep the path current if the folder was moved

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    s_main = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, APP_TITLE, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!s_main) return 1;
    EnableDarkMenus(s_main);

    s_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(s_main, s_wmTaskbarCreated, MSGFLT_ALLOW, nullptr);
    AddTrayIcon();
    Widget_Init(inst, s_main);
    Flyout_Init(inst, s_main);

    s_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    SetTimer(s_main, kUpdateTimer, kUpdateFirstDelayMs, nullptr);
    if (!s_updatedFrom.empty())
        ShowBalloon(L"Updated to " + std::wstring(AppVersion()),
                    L"Taskbar System Info was updated from " + s_updatedFrom + L" to " + AppVersion() + L".");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Cleanup();
    Gdiplus::GdiplusShutdown(gdipToken);
    CloseHandle(s_stopEvent);
    if (s_mutex) CloseHandle(s_mutex);
    return 0;
}
