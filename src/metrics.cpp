// System metric collection. Runs on the worker thread only.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include "app.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#include <cwchar>
#include <unordered_map>

// ---------------------------------------------------------------- enumeration helpers
std::vector<GpuInfo> EnumGpus() {
    std::vector<GpuInfo> out;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return out;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            bool dup = false;
            for (auto& e : out)
                if (e.luid.LowPart == d.AdapterLuid.LowPart && e.luid.HighPart == d.AdapterLuid.HighPart) dup = true;
            if (!dup) out.push_back({ d.Description, d.AdapterLuid, (uint64_t)d.DedicatedVideoMemory });
        }
        a->Release();
    }
    f->Release();
    return out;
}

static bool IsPhysical(const MIB_IF_ROW2& r) {
    return r.InterfaceAndOperStatusFlags.HardwareInterface && !r.InterfaceAndOperStatusFlags.FilterInterface &&
           r.Type != IF_TYPE_SOFTWARE_LOOPBACK;
}

std::vector<NetIfInfo> EnumNetInterfaces() {
    std::vector<NetIfInfo> out;
    PMIB_IF_TABLE2 t = nullptr;
    if (GetIfTable2(&t) != NO_ERROR) return out;
    for (ULONG i = 0; i < t->NumEntries; ++i) {
        const MIB_IF_ROW2& r = t->Table[i];
        if (r.InterfaceAndOperStatusFlags.FilterInterface || r.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            r.OperStatus != IfOperStatusUp || !r.Alias[0])
            continue;
        bool dup = false;
        for (auto& e : out) if (_wcsicmp(e.alias.c_str(), r.Alias) == 0) dup = true;
        if (!dup) out.push_back({ r.Alias, r.Description, IsPhysical(r) });
    }
    FreeMibTable(t);
    std::stable_sort(out.begin(), out.end(), [](const NetIfInfo& a, const NetIfInfo& b) { return a.physical > b.physical; });
    return out;
}

// ---------------------------------------------------------------- sampler
struct Sampler::Impl {
    // CPU
    ULONGLONG prevIdle = 0, prevTotal = 0;
    // GPU (PDH)
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER engine = nullptr, memory = nullptr;
    bool pdhTried = false;
    std::vector<BYTE> buf;
    std::unordered_map<std::wstring, double> engineSum;
    std::wstring key;
    std::vector<GpuInfo> gpus;
    ULONGLONG gpusTick = 0;
    // network
    std::unordered_map<ULONG64, std::pair<ULONG64, ULONG64>> netPrev, netNext;
    LARGE_INTEGER freq{}, netTime{};
    double totalDown = 0, totalUp = 0;
    std::wstring lastNetSel;

    ~Impl() { if (query) PdhCloseQuery(query); }

    static ULONGLONG FT(const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

    void Cpu(Sample& s) {
        FILETIME idle, kern, user;
        if (!GetSystemTimes(&idle, &kern, &user)) return;
        ULONGLONG i = FT(idle), t = FT(kern) + FT(user);   // kernel time includes idle time
        if (prevTotal && t > prevTotal) {
            double busy = 1.0 - double(i - prevIdle) / double(t - prevTotal);
            s.cpu = std::clamp(busy * 100.0, 0.0, 100.0);
        }
        prevIdle = i;
        prevTotal = t;
    }

    static void Ram(Sample& s) {
        MEMORYSTATUSEX ms{ sizeof(ms) };
        if (!GlobalMemoryStatusEx(&ms)) return;
        s.ramTotal = ms.ullTotalPhys;
        s.ramUsed = ms.ullTotalPhys - ms.ullAvailPhys;
        s.commitTotal = ms.ullTotalPageFile;
        s.commitUsed = ms.ullTotalPageFile - ms.ullAvailPageFile;
    }

    PDH_FMT_COUNTERVALUE_ITEM_W* Items(PDH_HCOUNTER c, DWORD fmt, DWORD& n) {
        n = 0;
        DWORD size = 0;
        if (PdhGetFormattedCounterArrayW(c, fmt, &size, &n, nullptr) != PDH_MORE_DATA || !size) return nullptr;
        buf.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(c, fmt, &size, &n, items) != ERROR_SUCCESS) { n = 0; return nullptr; }
        return items;
    }

    const GpuInfo* PickGpu(const std::wstring& name) {
        ULONGLONG now = GetTickCount64();
        if (!gpusTick || now - gpusTick > 5 * 60 * 1000) { gpus = EnumGpus(); gpusTick = now; }
        const GpuInfo* best = nullptr;
        for (auto& a : gpus) {
            if (!name.empty() && _wcsicmp(a.name.c_str(), name.c_str()) == 0) return &a;
            if (!best || a.dedicated > best->dedicated) best = &a;
        }
        return best;
    }

    void Gpu(Sample& s, const Config& c) {
        if (!pdhTried) {
            pdhTried = true;
            if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
                if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine) != ERROR_SUCCESS)
                    engine = nullptr;
                if (PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &memory) != ERROR_SUCCESS)
                    memory = nullptr;
                if (!engine && !memory) { PdhCloseQuery(query); query = nullptr; }
                else PdhCollectQueryData(query);  // prime rate counters
            } else {
                query = nullptr;
            }
        }
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) return;

        const GpuInfo* sel = PickGpu(c.gpuAdapter);
        if (sel) s.gpuName = sel->name;
        wchar_t luid[32] = {};
        if (sel) swprintf_s(luid, L"luid_0x%08X_0x%08X", (unsigned)sel->luid.HighPart, (unsigned)sel->luid.LowPart);
        const size_t luidLen = wcslen(luid);  // 26 when set

        // Instance names look like: pid_1234_luid_0x00000000_0x0000D9C6_phys_0_eng_3_engtype_3D
        // Like Task Manager: sum each engine over all processes, GPU usage = busiest engine.
        DWORD n = 0;
        if (engine) {
            if (auto* it = Items(engine, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, n)) {
                engineSum.clear();
                for (DWORD i = 0; i < n; ++i) {
                    if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                    const wchar_t* l = wcsstr(it[i].szName, L"luid_");
                    if (!l) continue;
                    const wchar_t* e = wcsstr(l, L"_engtype_");
                    key.assign(l, e ? size_t(e - l) : wcslen(l));
                    engineSum[key] += it[i].FmtValue.doubleValue;
                }
                double best = 0, bestAny = 0;
                bool matched = false;
                for (auto& kv : engineSum) {
                    bestAny = std::max(bestAny, kv.second);
                    if (luidLen && _wcsnicmp(kv.first.c_str(), luid, luidLen) == 0) {
                        best = std::max(best, kv.second);
                        matched = true;
                    }
                }
                s.gpu = std::clamp(matched ? best : bestAny, 0.0, 100.0);
                s.gpuOk = true;
            }
        }
        if (memory) {
            if (auto* it = Items(memory, PDH_FMT_LARGE, n)) {
                ULONGLONG used = 0, usedAny = 0;
                bool matched = false;
                for (DWORD i = 0; i < n; ++i) {
                    if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                    ULONGLONG v = (ULONGLONG)std::max<LONGLONG>(0, it[i].FmtValue.largeValue);
                    usedAny = std::max(usedAny, v);
                    if (luidLen && _wcsnicmp(it[i].szName, luid, luidLen) == 0) { used += v; matched = true; }
                }
                s.vramUsed = matched ? used : usedAny;
                s.vramTotal = sel ? sel->dedicated : 0;
                s.vramOk = s.vramTotal > 0;
            }
        }
    }

    void Net(Sample& s, const Config& c) {
        PMIB_IF_TABLE2 t = nullptr;
        if (GetIfTable2(&t) != NO_ERROR) return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);

        if (c.netInterface != lastNetSel) {  // selection changed: restart session totals
            lastNetSel = c.netInterface;
            totalDown = totalUp = 0;
        }
        ULONG64 din = 0, dout = 0;
        netNext.clear();
        for (ULONG i = 0; i < t->NumEntries; ++i) {
            const MIB_IF_ROW2& r = t->Table[i];
            bool match = c.netInterface.empty()
                ? IsPhysical(r)
                : (!r.InterfaceAndOperStatusFlags.FilterInterface && _wcsicmp(r.Alias, c.netInterface.c_str()) == 0);
            if (!match) continue;
            if (r.OperStatus == IfOperStatusUp && s.netName.find(r.Alias) == std::wstring::npos) {
                if (!s.netName.empty()) s.netName += L", ";
                s.netName += r.Alias;
            }
            netNext[r.InterfaceLuid.Value] = { r.InOctets, r.OutOctets };
            auto p = netPrev.find(r.InterfaceLuid.Value);
            if (p != netPrev.end() && r.InOctets >= p->second.first && r.OutOctets >= p->second.second) {
                din += r.InOctets - p->second.first;
                dout += r.OutOctets - p->second.second;
            }
        }
        FreeMibTable(t);

        if (netTime.QuadPart) {
            double dt = double(now.QuadPart - netTime.QuadPart) / double(freq.QuadPart);
            if (dt > 0.01) { s.netDown = din / dt; s.netUp = dout / dt; }
            totalDown += double(din);
            totalUp += double(dout);
        }
        s.netTotalDown = totalDown;
        s.netTotalUp = totalUp;
        if (s.netName.empty()) s.netName = c.netInterface.empty() ? L"No active adapter" : c.netInterface + L" (not connected)";
        netPrev.swap(netNext);
        netTime = now;
    }
};

Sampler::Sampler() : p_(new Impl) {
    Sample dummy;
    p_->Cpu(dummy);  // baseline for the first delta
}

Sampler::~Sampler() { delete p_; }

void Sampler::Collect(Sample& s, const Config& c) {
    p_->Cpu(s);
    p_->Ram(s);
    if (c.m[M_GPU].show || c.m[M_VRAM].show) p_->Gpu(s, c);
    p_->Net(s, c);
}
