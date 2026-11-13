// Finds free-space anchors inside the Windows 11 (XAML) taskbar via UI Automation:
// the Widgets/weather button and the left edge of the tray icons / clock.
// Works for the main taskbar and for secondary-monitor taskbars, which have no TrayNotifyWnd.
// Used on the worker thread only (COM initialized there).
#include "app.h"
#include <ole2.h>
#include <uiautomation.h>

struct TaskbarProbe::Impl {
    IUIAutomation* uia = nullptr;
    IUIAutomationCondition* cond = nullptr;
    IUIAutomationCacheRequest* cache = nullptr;

    IUIAutomationCondition* IdIs(const wchar_t* id) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(id);
        IUIAutomationCondition* c = nullptr;
        uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &c);
        VariantClear(&v);
        return c;
    }

    Impl() {
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
            uia = nullptr;
            return;
        }
        IUIAutomationCondition* c[3] = { IdIs(L"WidgetsButton"), IdIs(L"SystemTrayIcon"), IdIs(L"NotifyItemIcon") };
        if (c[0] && c[1] && c[2]) uia->CreateOrConditionFromNativeArray(c, 3, &cond);
        for (auto* x : c) if (x) x->Release();
        if (SUCCEEDED(uia->CreateCacheRequest(&cache))) {
            cache->AddProperty(UIA_BoundingRectanglePropertyId);
            cache->AddProperty(UIA_AutomationIdPropertyId);
        }
    }
    ~Impl() {
        if (cache) cache->Release();
        if (cond) cond->Release();
        if (uia) uia->Release();
    }
};

TaskbarProbe::TaskbarProbe() : p_(new Impl) {}
TaskbarProbe::~TaskbarProbe() { delete p_; }

TaskbarLayoutInfo TaskbarProbe::Probe(HWND tb) {
    TaskbarLayoutInfo info;
    info.taskbar = tb;
    if (!tb || !p_->uia || !p_->cond || !p_->cache) return info;
    if (!FindWindowExW(tb, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)) return info;  // classic

    IUIAutomationElement* root = nullptr;
    if (FAILED(p_->uia->ElementFromHandle(tb, &root)) || !root) return info;
    IUIAutomationElementArray* arr = nullptr;
    if (SUCCEEDED(root->FindAllBuildCache(TreeScope_Descendants, p_->cond, p_->cache, &arr)) && arr) {
        int n = 0;
        arr->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            IUIAutomationElement* el = nullptr;
            if (FAILED(arr->GetElement(i, &el)) || !el) continue;
            RECT r{};
            BSTR id = nullptr;
            if (SUCCEEDED(el->get_CachedBoundingRectangle(&r)) && r.right > r.left &&
                SUCCEEDED(el->get_CachedAutomationId(&id)) && id) {
                if (wcscmp(id, L"WidgetsButton") == 0) info.widgets = r;
                else info.trayLeft = std::min<int>(info.trayLeft, r.left);
            }
            if (id) SysFreeString(id);
            el->Release();
        }
        arr->Release();
    }
    root->Release();
    return info;
}
