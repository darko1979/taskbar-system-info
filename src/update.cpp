// Update check against the latest GitHub release (WinHTTP, background thread).
// Only the public GitHub API is contacted; nothing about the user or system is sent
// beyond what any HTTPS request carries (User-Agent "TaskbarSysInfo/<version>").
#include "app.h"
#include "../res/version.h"
#include <winhttp.h>
#include <cwchar>

namespace {

const wchar_t kHost[] = L"api.github.com";
#ifndef UPDATE_API_PATH  // overridable at build time for testing
#define UPDATE_API_PATH L"/repos/darko1979/taskbar-system-info/releases/latest"
#endif
const wchar_t kPath[] = UPDATE_API_PATH;
volatile LONG s_busy = 0;

struct Job { HWND notify; bool manual; };

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Minimal JSON helper: value of the first "key": "string" pair (top-level fields come first in GitHub responses).
std::string JsonString(const std::string& body, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return {};
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = body.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || body[p] != '"') return {};
    std::string out;
    for (++p; p < body.size() && body[p] != '"'; ++p) {
        if (body[p] == '\\' && p + 1 < body.size()) ++p;  // simple unescape (\" \\ \/)
        out += body[p];
    }
    return out;
}

std::wstring WinErr(const wchar_t* what) {
    DWORD e = GetLastError();
    wchar_t b[128];
    swprintf_s(b, L"%s failed (error %lu)", what, e);
    if (e == ERROR_WINHTTP_TIMEOUT) return L"The connection to GitHub timed out.";
    if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT)
        return L"Could not connect to GitHub. Check your internet connection.";
    return b;
}

bool HttpGet(std::string& body, DWORD& status, std::wstring& err) {
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"TaskbarSysInfo/" VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = nullptr, req = nullptr;
    if (!ses) { err = WinErr(L"WinHttpOpen"); return false; }
    WinHttpSetTimeouts(ses, 10000, 10000, 15000, 15000);
    con = WinHttpConnect(ses, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (con) req = WinHttpOpenRequest(con, L"GET", kPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        err = WinErr(L"WinHttpOpenRequest");
    } else if (!WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                                   (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
               !WinHttpReceiveResponse(req, nullptr)) {
        err = WinErr(L"Request");
    } else {
        DWORD sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sz, WINHTTP_NO_HEADER_INDEX);
        ok = true;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
            if (body.size() + avail > 4 * 1024 * 1024) break;  // sanity limit
            size_t old = body.size();
            body.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, &body[old], avail, &read)) { ok = false; err = WinErr(L"Read"); break; }
            body.resize(old + read);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// "v1.2.3" / "1.2" -> {1,2,3}
void ParseVersion(const std::wstring& s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    const wchar_t* p = s.c_str();
    while (*p && !iswdigit(*p)) ++p;
    for (int i = 0; i < 3 && *p; ++i) {
        v[i] = (int)wcstol(p, const_cast<wchar_t**>(&p), 10);
        if (*p != L'.') break;
        ++p;
    }
}

bool IsNewer(const std::wstring& tag) {
    int a[3], b[3];
    ParseVersion(tag, a);
    ParseVersion(VER_WSTR, b);
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

DWORD WINAPI CheckThread(void* p) {
    Job job = *static_cast<Job*>(p);
    delete static_cast<Job*>(p);
    auto* r = new UpdateResult;
    std::string body;
    DWORD status = 0;
    if (HttpGet(body, status, r->error)) {
        if (status == 200) {
            r->tag = Utf8ToWide(JsonString(body, "tag_name"));
            r->url = Utf8ToWide(JsonString(body, "html_url"));
            if (r->url.rfind(L"https://github.com/", 0) != 0)  // only ever open GitHub pages
                r->url = L"https://github.com/darko1979/taskbar-system-info/releases/latest";
            r->ok = !r->tag.empty();
            r->newer = r->ok && IsNewer(r->tag);
            if (!r->ok) r->error = L"Unexpected response from GitHub.";
        } else if (status == 404) {
            r->ok = true;
            r->noReleases = true;
        } else if (status == 403 || status == 429) {
            r->error = L"GitHub rate limit reached. Please try again later.";
        } else {
            r->error = L"GitHub returned HTTP " + std::to_wstring(status) + L".";
        }
    }
    InterlockedExchange(&s_busy, 0);
    if (!PostMessageW(job.notify, WM_APP_UPDATE, job.manual, (LPARAM)r)) delete r;
    return 0;
}

}  // namespace

const wchar_t* AppVersion() { return VER_WSTR; }

bool Update_InProgress() { return s_busy != 0; }

void Update_CheckAsync(HWND notify, bool manual) {
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return;  // one check at a time
    auto* job = new Job{ notify, manual };
    HANDLE t = CreateThread(nullptr, 0, CheckThread, job, 0, nullptr);
    if (t) CloseHandle(t);
    else { delete job; InterlockedExchange(&s_busy, 0); }
}
