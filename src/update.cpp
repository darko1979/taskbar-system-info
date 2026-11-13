// Update check against the latest GitHub release, and in-place self-update (WinHTTP, background threads).
// Only GitHub is contacted; nothing about the user or system is sent beyond what any HTTPS request
// carries (User-Agent "TaskbarSysInfo/<version>").
//
// Self-update: download the portable zip of the latest release, verify its SHA-256 (from the release
// asset digest or the checksum in the release notes), extract it with Windows' built-in tar.exe, check
// the new exe's version, then swap TaskbarSysInfo.exe (the running exe is renamed to .old, which Windows
// allows) and let the UI thread restart the app.
#include "app.h"
#include "../res/version.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <memory>
#include <shellapi.h>
#include <cwchar>
#include <vector>

namespace {

const wchar_t kHost[] = L"api.github.com";
#ifndef UPDATE_API_PATH  // overridable at build time for testing
#define UPDATE_API_PATH L"/repos/darko1979/taskbar-system-info/releases/latest"
#endif
const wchar_t kPath[] = UPDATE_API_PATH;
#ifndef UPDATE_DOWNLOAD_PREFIX
#define UPDATE_DOWNLOAD_PREFIX L"https://github.com/darko1979/taskbar-system-info/releases/download/"
#endif
const wchar_t kAssetSuffix[] = L"_x64_portable.zip";
const ULONGLONG kMaxDownload = 64ull * 1024 * 1024;
volatile LONG s_busy = 0;
volatile LONG s_installing = 0;

struct Job { HWND notify; bool manual; };
struct InstallJob { HWND notify; std::wstring tag, url, sha256; };

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Minimal JSON helper: value of the first "key": "string" pair at or after `from`
// (top-level fields come first in GitHub responses). *end receives the position after the value.
std::string JsonString(const std::string& body, const char* key, size_t from = 0, size_t* end = nullptr) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k, from);
    if (end) *end = std::string::npos;
    if (p == std::string::npos) return {};
    if (end) *end = p + k.size();
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = body.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || body[p] != '"') return {};
    std::string out;
    for (++p; p < body.size() && body[p] != '"'; ++p) {
        if (body[p] == '\\' && p + 1 < body.size()) ++p;  // simple unescape (\" \\ \/)
        out += body[p];
    }
    if (end) *end = p;
    return out;
}

bool IsHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char ch : s)
        if (!isxdigit((unsigned char)ch)) return false;
    return true;
}

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = towlower(ch);
    return s;
}

// Finds the portable zip asset and its SHA-256 in a release JSON.
void ParseAsset(const std::string& body, std::wstring& url, std::wstring& sha) {
    size_t from = body.find("\"assets\"");
    while (from != std::string::npos) {
        size_t at = 0;
        const std::wstring dl = Utf8ToWide(JsonString(body, "browser_download_url", from, &at));
        if (at == std::string::npos) break;
        if (dl.size() > wcslen(kAssetSuffix) &&
            _wcsicmp(dl.c_str() + dl.size() - wcslen(kAssetSuffix), kAssetSuffix) == 0) {
            url = dl;
            // "digest": "sha256:..." comes before browser_download_url within the same asset object
            size_t d = body.rfind("\"digest\"", at);
            if (d != std::string::npos && d >= from) {
                std::string v = JsonString(body, "digest", d);
                if (v.rfind("sha256:", 0) == 0 && IsHex64(v.substr(7))) sha = Lower(Utf8ToWide(v.substr(7)));
            }
            break;
        }
        from = at;
    }
    if (sha.empty()) {  // fallback: "SHA-256: `HEX`" in the release notes written by the release workflow
        size_t p = body.find("SHA-256: `");
        if (p != std::string::npos && IsHex64(body.substr(p + 10, 64))) sha = Lower(Utf8ToWide(body.substr(p + 10, 64)));
    }
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

// HTTPS GET; the response goes to `body` or, if `file` is valid, into that file. Redirects are followed.
bool HttpsGet(const wchar_t* host, INTERNET_PORT port, const wchar_t* path, const wchar_t* headers, std::string* body,
              HANDLE file, DWORD& status, std::wstring& err) {
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"TaskbarSysInfo/" VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = nullptr, req = nullptr;
    if (!ses) { err = WinErr(L"WinHttpOpen"); return false; }
    WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
    con = WinHttpConnect(ses, host, port, 0);
    if (con) req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        err = WinErr(L"WinHttpOpenRequest");
    } else if (!WinHttpSendRequest(req, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
               !WinHttpReceiveResponse(req, nullptr)) {
        err = WinErr(L"Request");
    } else {
        DWORD sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sz, WINHTTP_NO_HEADER_INDEX);
        ok = true;
        std::vector<char> buf(64 * 1024);
        ULONGLONG total = 0;
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &read)) { ok = false; err = WinErr(L"Download"); break; }
            if (!read) break;
            total += read;
            if (total > (body ? 4ull * 1024 * 1024 : kMaxDownload)) { ok = false; err = L"The download is unexpectedly large."; break; }
            if (body) {
                body->append(buf.data(), read);
            } else {
                DWORD wr = 0;
                if (!WriteFile(file, buf.data(), read, &wr, nullptr) || wr != read) { ok = false; err = WinErr(L"Writing the download"); break; }
            }
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

int CompareVersions(const std::wstring& x, const std::wstring& y) {
    int a[3], b[3];
    ParseVersion(x, a);
    ParseVersion(y, b);
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

DWORD WINAPI CheckThread(void* p) {
    Job job = *static_cast<Job*>(p);
    delete static_cast<Job*>(p);
    auto* r = new UpdateResult;
    std::string body;
    DWORD status = 0;
    if (HttpsGet(kHost, INTERNET_DEFAULT_HTTPS_PORT, kPath,
                 L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", &body, nullptr, status,
                 r->error)) {
        if (status == 200) {
            r->tag = Utf8ToWide(JsonString(body, "tag_name"));
            r->url = Utf8ToWide(JsonString(body, "html_url"));
            if (r->url.rfind(L"https://github.com/", 0) != 0)  // only ever open GitHub pages
                r->url = L"https://github.com/darko1979/taskbar-system-info/releases/latest";
            ParseAsset(body, r->assetUrl, r->sha256);
            if (r->assetUrl.rfind(UPDATE_DOWNLOAD_PREFIX, 0) != 0) r->assetUrl.clear();  // only this repo's releases
            r->ok = !r->tag.empty();
            r->newer = r->ok && CompareVersions(r->tag, VER_WSTR) > 0;
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

// ---------------------------------------------------------------- self-update helpers
std::wstring FileSha256(const std::wstring& path) {
    std::wstring out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<BYTE> buf(64 * 1024);
            DWORD n = 0;
            bool ok = true;
            while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) ok = BCryptHashData(h, buf.data(), n, 0) == 0;
            CloseHandle(f);
            BYTE d[32];
            if (ok && BCryptFinishHash(h, d, sizeof(d), 0) == 0) {
                wchar_t hx[65];
                for (int i = 0; i < 32; ++i) swprintf_s(hx + i * 2, 3, L"%02x", d[i]);
                out = hx;
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::wstring FileVersion(const std::wstring& path) {
    DWORD dummy = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!n) return {};
    std::vector<BYTE> b(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, n, b.data()) || !VerQueryValueW(b.data(), L"\\", (void**)&fi, &len) || !fi)
        return {};
    return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(fi->dwFileVersionLS));
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void DeleteTree(const std::wstring& dir) {
    std::wstring from = dir;
    from.push_back(L'\0');  // double-null terminated
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

bool RunHidden(const std::wstring& app, std::wstring cmd, DWORD timeoutMs, DWORD& exitCode) {
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    bool ok = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exitCode);
    if (!ok) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    return ok;
}

std::wstring ExePath() {
    wchar_t b[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, b, ARRAYSIZE(b));
    return (n && n < ARRAYSIZE(b)) ? std::wstring(b, n) : std::wstring();
}

bool Install(const InstallJob& job, std::wstring& err) {
    if (job.url.rfind(UPDATE_DOWNLOAD_PREFIX, 0) != 0) { err = L"The release has no portable x64 zip to download."; return false; }
    if (job.sha256.size() != 64) { err = L"The release has no SHA-256 checksum, so the download cannot be verified."; return false; }
    const std::wstring exe = ExePath();
    if (exe.empty()) { err = L"Could not determine the app's location."; return false; }

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (!FileExists(tar)) { err = L"This version of Windows has no built-in zip extractor (tar.exe)."; return false; }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"TaskbarSysInfo-update";
    DeleteTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring zip = dir + L"\\update.zip", out = dir + L"\\files";
    CreateDirectoryW(out.c_str(), nullptr);

    bool ok = false;
    // 1. download
    wchar_t host[256] = {}, path[2048] = {};
    URL_COMPONENTS uc{ sizeof(uc) };
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    HANDLE f = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!WinHttpCrackUrl(job.url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        err = L"Invalid download URL.";
    } else if (f == INVALID_HANDLE_VALUE) {
        err = WinErr(L"Creating the temporary file");
    } else {
        DWORD status = 0;
        ok = HttpsGet(host, uc.nPort, path, nullptr, nullptr, f, status, err);
        if (ok && status != 200) { ok = false; err = L"The download failed (HTTP " + std::to_wstring(status) + L")."; }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);

    // 2. verify, 3. extract, 4. check the new exe
    std::wstring newExe;
    if (ok && FileSha256(zip) != job.sha256) { ok = false; err = L"The downloaded file failed the SHA-256 check."; }
    if (ok) {
        DWORD code = 1;
        if (!RunHidden(tar, L"tar.exe -xf \"" + zip + L"\" -C \"" + out + L"\"", 60000, code) || code != 0) {
            ok = false;
            err = L"Could not extract the downloaded zip.";
        }
    }
    if (ok) {
        newExe = out + L"\\TaskbarSysInfo\\TaskbarSysInfo.exe";
        if (!FileExists(newExe)) newExe = out + L"\\TaskbarSysInfo.exe";
        if (!FileExists(newExe)) { ok = false; err = L"The zip does not contain TaskbarSysInfo.exe."; }
        else if (CompareVersions(FileVersion(newExe), job.tag) != 0) { ok = false; err = L"The downloaded exe has an unexpected version."; }
    }

    // 5. swap the exe: a running exe can be renamed, then the new one is copied into place
    if (ok) {
        const std::wstring old = exe + L".old";
        DeleteFileW(old.c_str());
        if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            ok = false;
            err = L"Cannot replace TaskbarSysInfo.exe (" + WinErr(L"Rename") +
                  L"). The folder may be read-only or need administrator rights.";
        } else if (!CopyFileW(newExe.c_str(), exe.c_str(), FALSE)) {
            ok = false;
            err = WinErr(L"Copying the new TaskbarSysInfo.exe");
            MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);  // roll back
        } else {
            // keep the README next to the exe current, if there is one
            const std::wstring exeDir = exe.substr(0, exe.find_last_of(L'\\'));
            const std::wstring newReadme = newExe.substr(0, newExe.find_last_of(L'\\')) + L"\\README.md";
            if (FileExists(exeDir + L"\\README.md") && FileExists(newReadme))
                CopyFileW(newReadme.c_str(), (exeDir + L"\\README.md").c_str(), FALSE);
        }
    }
    DeleteTree(dir);
    return ok;
}

DWORD WINAPI InstallThread(void* p) {
    std::unique_ptr<InstallJob> job(static_cast<InstallJob*>(p));
    auto* r = new UpdateResult;
    r->tag = job->tag;
    r->ok = Install(*job, r->error);
    InterlockedExchange(&s_installing, 0);
    if (!PostMessageW(job->notify, WM_APP_INSTALLED, 0, (LPARAM)r)) delete r;
    return 0;
}

}  // namespace

const wchar_t* AppVersion() { return VER_WSTR; }

bool Update_InProgress() { return s_busy != 0; }
bool Update_Installing() { return s_installing != 0; }

void Update_CheckAsync(HWND notify, bool manual) {
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return;  // one check at a time
    auto* job = new Job{ notify, manual };
    HANDLE t = CreateThread(nullptr, 0, CheckThread, job, 0, nullptr);
    if (t) CloseHandle(t);
    else { delete job; InterlockedExchange(&s_busy, 0); }
}

void Update_InstallAsync(HWND notify, const std::wstring& tag, const std::wstring& assetUrl, const std::wstring& sha256) {
    if (InterlockedCompareExchange(&s_installing, 1, 0) != 0) return;
    auto* job = new InstallJob{ notify, tag, assetUrl, sha256 };
    HANDLE t = CreateThread(nullptr, 0, InstallThread, job, 0, nullptr);
    if (t) CloseHandle(t);
    else { delete job; InterlockedExchange(&s_installing, 0); }
}

void Update_CleanupOld() {
    const std::wstring exe = ExePath();
    if (!exe.empty()) DeleteFileW((exe + L".old").c_str());
}
