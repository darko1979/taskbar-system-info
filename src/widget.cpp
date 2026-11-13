// Widget windows embedded into the taskbar(s): Shell_TrayWnd (main) and Shell_SecondaryTrayWnd (other monitors).
// Each is a per-pixel-alpha layered child window rendered with GDI+ into a DIB.
#include "app.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <climits>
#include <cmath>
#include <memory>

using namespace Gdiplus;

namespace {

const wchar_t kClass[] = L"TaskbarSysInfoWidget";

struct Tile {
    Metric m;
    bool hasGraph;
    RECT graph;
    RectF text;   // two-line text block
};

// One widget per taskbar.
struct Inst {
    HWND taskbar = nullptr, hwnd = nullptr, tray = nullptr, band = nullptr;
    bool primary = false, win11 = false, embedded = false, embedFailed = false;
    bool dirty = true, hover = false, pressed = false;
    bool clickThrough = false;   // window created with WS_EX_TRANSPARENT
    RECT tbRect{};
    UINT dpi = 96;
    float scale = 1.f;
    // classic taskbar: task-button band (ReBarWindow32 / WorkerW) we shrink to make room
    bool bandAdjusted = false;
    int natStart = INT_MIN, expectStart = INT_MIN;
    // layout
    std::vector<Tile> tiles;
    int w = 0, h = 0;
    bool vertical = false;
    std::unique_ptr<Font> font;
    float lineH = 16.f;
    // backing store
    HDC memDC = nullptr;
    HBITMAP dib = nullptr, oldBmp = nullptr;
    void* bits = nullptr;
    int dibW = 0, dibH = 0;

    int S(float v) const { return (int)std::lround(v * scale); }
};

HINSTANCE s_hinst;
HWND s_owner;
std::vector<std::unique_ptr<Inst>> s_list;
bool s_layoutAll = true;
COLORREF s_textColor = RGB(255, 255, 255);

std::vector<float> s_cols, s_cols2;
std::vector<PointF> s_pts;

Color ToColor(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

Config CopyConfig() {
    AcquireSRWLockShared(&g_shared.lock);
    Config c = g_shared.cfg;
    ReleaseSRWLockShared(&g_shared.lock);
    return c;
}

void Render(Inst& w, const Config& c);

// ---------------------------------------------------------------- window procedure
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Inst* w = reinterpret_cast<Inst*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return w && w->clickThrough ? HTTRANSPARENT : HTCLIENT;
    case WM_MOUSEMOVE:
        if (w && !w->hover) {
            w->hover = true;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            Render(*w, CopyConfig());
        }
        return 0;
    case WM_MOUSELEAVE:
        if (w) {
            w->hover = w->pressed = false;
            Render(*w, CopyConfig());
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (w) {
            w->pressed = true;
            SetCapture(h);
            Render(*w, CopyConfig());
        }
        return 0;
    case WM_LBUTTONUP:
        if (w && w->pressed) {
            w->pressed = false;
            ReleaseCapture();
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            RECT rc;
            GetClientRect(h, &rc);
            Render(*w, CopyConfig());
            if (PtInRect(&rc, pt)) {
                RECT wr;
                GetWindowRect(h, &wr);
                Flyout_Toggle(wr, w->taskbar);
            }
        }
        return 0;
    case WM_RBUTTONUP:
        Flyout_Hide();
        PostMessageW(s_owner, WM_APP_MENU, 0, 0);
        return 0;
    case WM_NCDESTROY:
        if (w && w->hwnd == h) w->hwnd = nullptr;
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- classic taskbar band handling
void MoveBand(Inst& w, int l, int t, int r, int b) {  // screen coordinates
    POINT a{ l, t }, z{ r, b };
    ScreenToClient(w.taskbar, &a);
    ScreenToClient(w.taskbar, &z);
    MoveWindow(w.band, a.x, a.y, z.x - a.x, z.y - a.y, TRUE);
    w.bandAdjusted = true;
}

void RestoreBand(Inst& w) {
    if (!w.bandAdjusted || !w.band || !IsWindow(w.band)) {
        w.bandAdjusted = false;
        return;
    }
    RECT rb, tr{};
    GetWindowRect(w.band, &rb);
    bool tray = w.tray && GetWindowRect(w.tray, &tr);
    if (!w.vertical) {
        int l = w.natStart != INT_MIN ? w.natStart : rb.left;
        MoveBand(w, l, rb.top, tray ? tr.left : rb.right, rb.bottom);
    } else {
        int t = w.natStart != INT_MIN ? w.natStart : rb.top;
        MoveBand(w, rb.left, t, rb.right, tray ? tr.top : rb.bottom);
    }
    w.bandAdjusted = false;
    w.natStart = w.expectStart = INT_MIN;
}

// ---------------------------------------------------------------- window creation / attachment
void FreeInst(Inst& w) {
    if (w.hwnd && IsWindow(w.hwnd)) {
        RestoreBand(w);
        HWND h = w.hwnd;
        w.hwnd = nullptr;
        DestroyWindow(h);
    }
    w.hwnd = nullptr;
    if (w.memDC) {
        if (w.dib) {
            SelectObject(w.memDC, w.oldBmp);
            DeleteObject(w.dib);
        }
        DeleteDC(w.memDC);
    }
    w.memDC = nullptr;
    w.dib = nullptr;
    w.font.reset();
}

bool EnsureWindow(Inst& w) {
    if (w.hwnd && IsWindow(w.hwnd)) {
        if (!w.embedded || GetParent(w.hwnd) == w.taskbar) return true;
        HWND h = w.hwnd;
        w.hwnd = nullptr;
        DestroyWindow(h);
    }
    w.bandAdjusted = false;
    w.natStart = w.expectStart = INT_MIN;
    HWND tb = w.taskbar;
    w.primary = tb && tb == FindWindowW(L"Shell_TrayWnd", nullptr);
    w.win11 = tb && FindWindowExW(tb, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
    if (w.primary) {
        w.tray = FindWindowExW(tb, nullptr, L"TrayNotifyWnd", nullptr);
        w.band = FindWindowExW(tb, nullptr, L"ReBarWindow32", nullptr);
    } else if (tb) {  // classic secondary taskbar: clock button + WorkerW band
        w.tray = FindWindowExW(tb, nullptr, L"ClockButton", nullptr);
        w.band = FindWindowExW(tb, nullptr, L"WorkerW", nullptr);
    }

    const DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY |
                     (w.clickThrough ? WS_EX_TRANSPARENT : 0);
    w.embedded = false;
    if (tb && !w.embedFailed) {
        w.hwnd = CreateWindowExW(ex, kClass, APP_TITLE, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, tb, nullptr, s_hinst, &w);
        w.embedded = w.hwnd != nullptr;
        w.embedFailed = !w.embedded;
    }
    if (!w.hwnd)  // fallback: a topmost popup laid over the taskbar
        w.hwnd = CreateWindowExW(ex | WS_EX_TOPMOST, kClass, APP_TITLE, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, s_hinst, &w);
    w.dirty = true;
    return w.hwnd != nullptr;
}

// ---------------------------------------------------------------- layout
void TileText(Metric m, const Sample& s, const Config& c, std::wstring& l1, std::wstring& l2) {
    switch (m) {
    case M_CPU:
        l1 = L"CPU";
        l2 = FormatPct(s.cpu);
        break;
    case M_GPU:
        l1 = L"GPU";
        l2 = s.gpuOk ? FormatPct(s.gpu) : L"n/a";
        break;
    case M_RAM:
        l1 = L"RAM " + FormatPct(s.ramTotal ? 100.0 * s.ramUsed / s.ramTotal : 0);
        l2 = FormatSize(s.ramUsed);
        break;
    case M_VRAM:
        l1 = s.vramOk ? L"VRAM " + FormatPct(100.0 * s.vramUsed / s.vramTotal) : L"VRAM";
        l2 = s.vramOk || s.vramUsed ? FormatSize(s.vramUsed) : L"n/a";
        break;
    default:
        l1 = L"\x2191 " + FormatRate(s.netUp, c.netBits);
        l2 = L"\x2193 " + FormatRate(s.netDown, c.netBits);
        break;
    }
}

void Layout(Inst& w, const Config& c) {
    w.scale = w.dpi / 96.f;
    const int tbW = w.tbRect.right - w.tbRect.left, tbH = w.tbRect.bottom - w.tbRect.top;
    w.vertical = tbH > tbW;

    const float px = c.fontSize * w.dpi / 72.f;
    w.font = std::make_unique<Font>(c.fontName.c_str(), px, FontStyleRegular, UnitPixel);
    if (w.font->GetLastStatus() != Ok) w.font = std::make_unique<Font>(L"Segoe UI", px, FontStyleRegular, UnitPixel);

    Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Graphics mg(&tmp);
    mg.SetTextRenderingHint(TextRenderingHintAntiAlias);
    w.lineH = w.font->GetHeight(&mg);
    auto measure = [&](const wchar_t* t) {
        RectF r;
        mg.MeasureString(t, -1, w.font.get(), PointF(0, 0), StringFormat::GenericTypographic(), &r);
        return r.Width;
    };
    auto textWidth = [&](Metric m) {
        static const wchar_t* const kTpl[M_COUNT][3] = {
            { L"CPU", L"100%", nullptr },
            { L"GPU", L"100%", nullptr },
            { L"RAM 100%", L"88.8 GB", L"888 MB" },
            { L"VRAM 100%", L"88.8 GB", L"888 MB" },
            { L"\x2193 88.8 MB/s", L"\x2193 888 KB/s", L"\x2193 88.8 Mb/s" },
        };
        float tw = 0;
        for (auto* t : kTpl[m]) if (t) tw = std::max(tw, measure(t));
        return (int)std::ceil(tw) + 1;
    };

    w.tiles.clear();
    const int pad = w.S(6), gap = w.S(6), sp = w.S((float)c.spacing);
    const float textH = w.lineH * 2;
    if (!w.vertical) {
        const int H = tbH;
        int gh = std::min(H - w.S(14), w.S(30));
        if (gh < 8) gh = std::max(H - 4, 4);
        int x = pad;
        for (int i = 0; i < M_COUNT; ++i) {
            if (!c.m[i].show) continue;
            Tile t{ (Metric)i, c.m[i].graph, {}, {} };
            int gw = t.hasGraph ? w.S((float)(c.m[i].graphWidth > 0 ? c.m[i].graphWidth : c.graphWidth)) : 0;
            int tw = textWidth(t.m);
            if (t.hasGraph) t.graph = { x, (H - gh) / 2, x + gw, (H - gh) / 2 + gh };
            int tx = x + (t.hasGraph ? gw + gap : 0);
            t.text = RectF((REAL)tx, (H - textH) / 2.f, (REAL)tw, textH);
            x = tx + tw + sp;
            w.tiles.push_back(t);
        }
        w.w = w.tiles.empty() ? 0 : x - sp + pad;
        w.h = H;
    } else {
        const int W = tbW, inner = W - 2 * pad, gh = w.S(22);
        int y = pad;
        for (int i = 0; i < M_COUNT; ++i) {
            if (!c.m[i].show) continue;
            Tile t{ (Metric)i, c.m[i].graph, {}, {} };
            t.text = RectF((REAL)pad, (REAL)y, (REAL)inner, textH);
            y += (int)std::ceil(textH);
            if (t.hasGraph) {
                t.graph = { pad, y + gap / 2, pad + inner, y + gap / 2 + gh };
                y += gh + gap;
            }
            y += sp;
            w.tiles.push_back(t);
        }
        w.w = W;
        w.h = w.tiles.empty() ? 0 : y - sp + pad;
    }
}

// ---------------------------------------------------------------- placement
void Place(Inst& w, const Config& c) {
    const RECT tb = w.tbRect;
    const int ofs = w.S((float)c.offset), margin = w.S(4);
    TaskbarLayoutInfo li;
    AcquireSRWLockShared(&g_shared.lock);
    for (auto& x : g_shared.layout)
        if (x.taskbar == w.taskbar) li = x;
    ReleaseSRWLockShared(&g_shared.lock);

    RECT tr{};
    bool tray = w.tray && GetWindowRect(w.tray, &tr) && tr.right > tr.left;
    if (!tray && w.win11 && li.trayLeft != INT32_MAX) {  // XAML tray (secondary taskbars): from UI Automation
        tr = { li.trayLeft, tb.top, tb.right, tb.bottom };
        tray = true;
    }
    const bool classic = w.embedded && !w.win11 && w.band && IsWindow(w.band);
    RECT rb{};
    if (classic) GetWindowRect(w.band, &rb);
    const RECT wb = li.widgets;
    const bool hasWb = w.win11 && wb.right > wb.left;
    int x, y;

    if (!w.vertical) {
        y = tb.top + ((tb.bottom - tb.top) - w.h) / 2;
        const int center = (tb.left + tb.right) / 2;
        if (c.position == 0) {
            int right = tray ? tr.left : tb.right;
            // Win11 left-aligned taskbar puts the Widgets/weather button next to the tray: keep clear of it.
            if (hasWb && wb.left > center && wb.left < right) right = wb.left;
            x = right - w.w - ofs - margin;
            if (classic && rb.right != x && x > rb.left) MoveBand(w, rb.left, rb.top, x, rb.bottom);
        } else if (classic) {
            if (rb.left != w.expectStart) w.natStart = rb.left;  // explorer re-laid out the band
            x = w.natStart + ofs;
            w.expectStart = x + w.w + margin;
            if (rb.left != w.expectStart && w.expectStart < rb.right) MoveBand(w, w.expectStart, rb.top, rb.right, rb.bottom);
        } else {
            int left = tb.left;
            if (hasWb && wb.right <= center) left = wb.right;  // centered taskbar: Widgets button is at the left
            x = left + ofs + margin;
        }
    } else {
        x = tb.left + ((tb.right - tb.left) - w.w) / 2;
        if (c.position == 0) {
            int bottom = tray ? tr.top : tb.bottom;
            y = bottom - w.h - ofs - margin;
            if (classic && rb.bottom != y && y > rb.top) MoveBand(w, rb.left, rb.top, rb.right, y);
        } else if (classic) {
            if (rb.top != w.expectStart) w.natStart = rb.top;
            y = w.natStart + ofs;
            w.expectStart = y + w.h + margin;
            if (rb.top != w.expectStart && w.expectStart < rb.bottom) MoveBand(w, rb.left, w.expectStart, rb.right, rb.bottom);
        } else {
            y = tb.top + ofs + margin;
        }
    }

    POINT p{ x, y };
    RECT cur;
    GetWindowRect(w.hwnd, &cur);
    POINT cp{ cur.left, cur.top };
    if (w.embedded) {
        ScreenToClient(w.taskbar, &p);
        ScreenToClient(w.taskbar, &cp);
    }
    const bool changed = cp.x != p.x || cp.y != p.y || cur.right - cur.left != w.w || cur.bottom - cur.top != w.h;
    const bool covered = w.embedded && GetWindow(w.hwnd, GW_HWNDPREV) != nullptr;  // something above us in z-order
    if (changed || covered || !IsWindowVisible(w.hwnd) || !w.embedded)
        SetWindowPos(w.hwnd, w.embedded ? HWND_TOP : HWND_TOPMOST, p.x, p.y, w.w, w.h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | (changed ? 0 : SWP_NOMOVE | SWP_NOSIZE));
}

// ---------------------------------------------------------------- rendering
void EnsureDib(Inst& w) {
    if (w.dib && w.w == w.dibW && w.h == w.dibH) return;
    if (!w.memDC) w.memDC = CreateCompatibleDC(nullptr);
    if (w.dib) {
        SelectObject(w.memDC, w.oldBmp);
        DeleteObject(w.dib);
        w.dib = nullptr;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w.w;
    bi.bmiHeader.biHeight = -w.h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    w.dib = CreateDIBSection(w.memDC, &bi, DIB_RGB_COLORS, &w.bits, nullptr, 0);
    w.oldBmp = (HBITMAP)SelectObject(w.memDC, w.dib);
    w.dibW = w.w;
    w.dibH = w.h;
}

// Reduce a history to one value per pixel column (max of the samples in the column).
// The newest sample is at the right edge; columns without data get -1.
void Columns(const History& h, int W, std::vector<float>& out) {
    out.assign(W > 0 ? W : 0, -1.f);
    const size_t C = h.Capacity(), N = h.Count();
    if (!C || !N || W <= 0) return;
    const size_t missing = C - N;
    for (int x = 0; x < W; ++x) {
        size_t j0 = size_t(uint64_t(x) * C / W), j1 = size_t(uint64_t(x + 1) * C / W);
        if (j1 <= j0) j1 = j0 + 1;
        float v = -1.f;
        for (size_t j = std::max(j0, missing); j < j1; ++j) v = std::max(v, h.At(j - missing));
        out[x] = v;
    }
}

void DrawSeries(Graphics& gr, const RectF& r, const std::vector<float>& cols, float scale, COLORREF color, bool fill, float penW) {
    s_pts.clear();
    const float bottom = r.Y + r.Height;
    for (size_t x = 0; x < cols.size(); ++x) {
        if (cols[x] < 0) continue;
        float v = std::clamp(cols[x] / scale, 0.f, 1.f);
        s_pts.emplace_back(r.X + x + 0.5f, bottom - v * r.Height);
    }
    if (s_pts.empty()) return;
    if (s_pts.size() == 1) s_pts.emplace_back(s_pts[0].X + 0.5f, s_pts[0].Y);
    if (fill) {
        float x0 = s_pts.front().X - 0.5f, x1 = s_pts.back().X + 0.5f;
        s_pts.emplace_back(x1, bottom);
        s_pts.emplace_back(x0, bottom);
        SolidBrush b(ToColor(color, 96));
        gr.FillPolygon(&b, s_pts.data(), (INT)s_pts.size());
        s_pts.resize(s_pts.size() - 2);
    }
    Pen pen(ToColor(color), penW);
    pen.SetLineJoin(LineJoinRound);
    gr.DrawLines(&pen, s_pts.data(), (INT)s_pts.size());
}

void Rounded(GraphicsPath& path, float x, float y, float w, float h, float rad) {
    const float d = rad * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void Render(Inst& w, const Config& c) {
    if (!w.hwnd || w.w <= 0 || w.h <= 0 || !w.font) return;
    EnsureDib(w);
    if (!w.bits) return;
    {
        Bitmap bmp(w.w, w.h, w.w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(w.bits));
        Graphics gr(&bmp);
        gr.SetCompositingMode(CompositingModeSourceCopy);
        // alpha 1: invisible but still receives mouse clicks; alpha 0 lets clicks fall through to the taskbar
        gr.Clear(Color(c.clickThrough ? 0 : 1, 0, 0, 0));
        gr.SetCompositingMode(CompositingModeSourceOver);
        gr.SetPixelOffsetMode(PixelOffsetModeHalf);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);

        // Background plate and Win11-style hover / pressed highlight (rounded, like taskbar buttons).
        const float inset = w.vertical ? 0.f : (float)w.S(4);
        const float rad = (float)w.S(4);
        if (c.bgAlpha > 0 || w.hover || w.pressed) {
            GraphicsPath path;
            Rounded(path, 0.5f, inset, w.w - 1.f, w.h - 2 * inset, rad);
            if (c.bgAlpha > 0) {
                SolidBrush b(ToColor(c.bgColor, (BYTE)c.bgAlpha));
                gr.FillPath(&b, &path);
            }
            if (w.hover || w.pressed) {
                const bool light = LightTaskbarTheme();
                SolidBrush hb(light ? Color(w.pressed ? 10 : 18, 0, 0, 0) : Color(w.pressed ? 10 : 20, 255, 255, 255));
                gr.FillPath(&hb, &path);
                Pen hp(light ? Color(20, 0, 0, 0) : Color(24, 255, 255, 255), 1.f);
                gr.DrawPath(&hp, &path);
            }
        }

        gr.SetTextRenderingHint(TextRenderingHintAntiAlias);
        StringFormat fmt(StringFormat::GenericTypographic());
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap);
        if (w.vertical) fmt.SetAlignment(StringAlignmentCenter);
        SolidBrush dim(ToColor(s_textColor, w.pressed ? 150 : 200)), bright(ToColor(s_textColor, w.pressed ? 200 : 255));

        AcquireSRWLockShared(&g_shared.lock);
        const Sample s = g_shared.cur;
        std::wstring l1, l2;
        for (const Tile& t : w.tiles) {
            if (t.hasGraph) {
                const RECT& g0 = t.graph;
                gr.SetSmoothingMode(SmoothingModeNone);
                SolidBrush bg(ToColor(s_textColor, 24));
                gr.FillRectangle(&bg, (INT)g0.left, (INT)g0.top, (INT)(g0.right - g0.left), (INT)(g0.bottom - g0.top));
                Pen border(ToColor(s_textColor, 64), 1.f);
                gr.DrawRectangle(&border, (INT)g0.left, (INT)g0.top, (INT)(g0.right - g0.left - 1), (INT)(g0.bottom - g0.top - 1));
                RectF r((REAL)g0.left + 1, (REAL)g0.top + 1, (REAL)(g0.right - g0.left - 2), (REAL)(g0.bottom - g0.top - 2));
                const int W = (int)r.Width;
                const float penW = std::max(1.f, w.scale);
                gr.SetSmoothingMode(SmoothingModeAntiAlias);
                if (t.m == M_NET) {
                    Columns(g_shared.hist[S_DOWN], W, s_cols);
                    Columns(g_shared.hist[S_UP], W, s_cols2);
                    float mx = 0;
                    for (float v : s_cols) mx = std::max(mx, v);
                    for (float v : s_cols2) mx = std::max(mx, v);
                    const float scale = NiceRateScale(mx);
                    DrawSeries(gr, r, s_cols, scale, c.m[M_NET].color, true, penW);
                    DrawSeries(gr, r, s_cols2, scale, c.netUpColor, false, penW);
                } else {
                    static const Series kSer[] = { S_CPU, S_GPU, S_RAM, S_VRAM };
                    Columns(g_shared.hist[kSer[t.m]], W, s_cols);
                    DrawSeries(gr, r, s_cols, 100.f, c.m[t.m].color, true, penW);
                }
            }
            TileText(t.m, s, c, l1, l2);
            RectF a(t.text.X, t.text.Y, t.text.Width + w.S(20), w.lineH);
            RectF b(t.text.X, t.text.Y + w.lineH, t.text.Width + w.S(20), w.lineH);
            if (w.vertical) a.Width = b.Width = t.text.Width;
            gr.DrawString(l1.c_str(), -1, w.font.get(), a, &fmt, &dim);
            gr.DrawString(l2.c_str(), -1, w.font.get(), b, &fmt, &bright);
        }
        ReleaseSRWLockShared(&g_shared.lock);
    }
    SIZE sz{ w.w, w.h };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(w.hwnd, nullptr, nullptr, &sz, w.memDC, &src, 0, &bf, ULW_ALPHA);
}

void UpdateInst(Inst& w, const Config& c) {
    if (w.hwnd && w.clickThrough != c.clickThrough) {  // style changes need a new window
        RestoreBand(w);
        HWND h = w.hwnd;
        w.hwnd = nullptr;
        DestroyWindow(h);
    }
    w.clickThrough = c.clickThrough;
    if (w.clickThrough) w.hover = w.pressed = false;
    if (!EnsureWindow(w)) return;
    RECT tb{};
    UINT dpi = 96;
    if (w.taskbar && GetWindowRect(w.taskbar, &tb)) {
        dpi = GetDpiForWindow(w.taskbar);
    } else {  // no taskbar at all: use a strip at the bottom of the primary monitor
        dpi = GetDpiForSystem();
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        tb = mi.rcMonitor;
        tb.top = tb.bottom - MulDiv(48, dpi, 96);
    }
    if (!dpi) dpi = 96;
    if (!EqualRect(&tb, &w.tbRect) || dpi != w.dpi) {
        w.tbRect = tb;
        w.dpi = dpi;
        w.dirty = true;
    }
    if (w.dirty) {
        RestoreBand(w);
        Layout(w, c);
        w.dirty = false;
    }
    if (w.tiles.empty() || w.w <= 0 || w.h <= 0) {
        RestoreBand(w);
        ShowWindow(w.hwnd, SW_HIDE);
        return;
    }
    Render(w, c);
    Place(w, c);
}

}  // namespace

// ---------------------------------------------------------------- public API
void HistoryColumns(const History& h, int W, std::vector<float>& out) { Columns(h, W, out); }

void DrawGraphSeries(Graphics& gr, const RectF& r, const std::vector<float>& cols, float scale, COLORREF color, bool fill,
                     float penW) {
    DrawSeries(gr, r, cols, scale, color, fill, penW);
}

float NiceRateScale(float v) {
    v = std::max(v, 10.f * 1024.f);  // never scale below 10 KB/s
    float p = std::pow(10.f, std::floor(std::log10(v)));
    for (float m : { 1.f, 2.f, 5.f, 10.f })
        if (v <= m * p) return m * p;
    return 10.f * p;
}

bool LightTaskbarTheme() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

std::vector<HWND> FindTaskbars(int mode) {
    std::vector<HWND> out;
    HWND main = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (mode != 1 && main) out.push_back(main);
    if (mode != 0)
        for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
            if (IsWindowVisible(h)) out.push_back(h);
    if (out.empty() && main) out.push_back(main);  // "second taskbar" chosen but only one monitor
    return out;
}

void Widget_Init(HINSTANCE inst, HWND owner) {
    s_hinst = inst;
    s_owner = owner;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

void Widget_Update() {
    const Config c = CopyConfig();
    if (s_layoutAll) {
        s_textColor = c.textAuto ? (LightTaskbarTheme() ? RGB(0x10, 0x10, 0x10) : RGB(0xFF, 0xFF, 0xFF)) : c.textColor;
        for (auto& w : s_list) w->dirty = true;
        s_layoutAll = false;
    }

    // Reconcile widget instances with the taskbars that should carry one.
    std::vector<HWND> want = FindTaskbars(c.taskbars);
    if (want.empty()) want.push_back(nullptr);  // no explorer: single popup
    for (size_t i = 0; i < s_list.size();) {
        Inst& w = *s_list[i];
        bool keep = std::find(want.begin(), want.end(), w.taskbar) != want.end() && (!w.taskbar || IsWindow(w.taskbar));
        if (keep) { ++i; continue; }
        FreeInst(w);
        s_list.erase(s_list.begin() + i);
    }
    for (HWND tb : want) {
        bool have = false;
        for (auto& w : s_list) have |= w->taskbar == tb;
        if (!have) {
            auto w = std::make_unique<Inst>();
            w->taskbar = tb;
            s_list.push_back(std::move(w));
        }
    }
    for (auto& w : s_list) UpdateInst(*w, c);
}

void Widget_Invalidate() {
    s_layoutAll = true;
    for (auto& w : s_list) RestoreBand(*w);
}

void Widget_Reattach() {
    for (auto& w : s_list) FreeInst(*w);
    s_list.clear();
    s_layoutAll = true;
    Widget_Update();
}

void Widget_Shutdown() {
    for (auto& w : s_list) FreeInst(*w);
    s_list.clear();
}
