// Restock - running winget (see winget.h)
#include "winget.h"
#include "util.h"
#include "net.h"
#include "json.h"
#include <shlobj.h>
#include <mutex>

namespace winget {

static std::mutex s_lock;
static std::wstring s_path, s_version;
static int s_noInteractivityFlag = -1;      // -1 unknown, 1 supported, 0 an old winget without it

std::wstring Path() {
    std::lock_guard<std::mutex> g(s_lock);
    if (!s_path.empty() && FileExists(s_path)) return s_path;
    s_path.clear();
    // the App Installer alias; it's a zero-byte reparse point, which CreateProcess knows how to start
    std::wstring alias = KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\WindowsApps\\winget.exe";
    if (FileExists(alias)) { s_path = alias; return s_path; }
    wchar_t buf[MAX_PATH * 2];
    if (SearchPathW(nullptr, L"winget.exe", nullptr, MAX_PATH * 2, buf, nullptr)) s_path = buf;
    return s_path;
}

static std::wstring Decode(const std::string& bytes) {
    if (bytes.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) { cp = GetOEMCP(); n = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0); }
    std::wstring w(n, 0);
    MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), &w[0], n);
    return w;
}

static std::wstring CommonArgs() {
    std::wstring a = L" --accept-source-agreements";
    if (s_noInteractivityFlag != 0) a += L" --disable-interactivity";
    return a;
}

// Runs winget and returns its decoded output. Retries once without --disable-interactivity on a
// winget too old to know it.
static bool Run(const std::wstring& args, DWORD timeoutMs, std::wstring& out, DWORD& code, std::wstring* error) {
    std::wstring exe = Path();
    if (exe.empty()) { if (error) *error = L"winget isn't installed."; return false; }
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string bytes;
        if (!RunCapture(exe, args + CommonArgs(), timeoutMs, [&](const std::string& b) { bytes += b; return bytes.size() < 64u * 1024 * 1024; }, &code)) {
            if (error) *error = L"winget couldn't start (error " + std::to_wstring(GetLastError()) + L").";
            return false;
        }
        out = Decode(bytes);
        if (code != 0 && s_noInteractivityFlag != 0 && IContains(out, L"--disable-interactivity")) {
            Log(L"winget doesn't know --disable-interactivity; retrying without it");
            s_noInteractivityFlag = 0;
            continue;
        }
        if (s_noInteractivityFlag < 0) s_noInteractivityFlag = 1;
        if (code == STILL_ACTIVE) { if (error) *error = L"winget took too long to answer."; return false; }
        return true;
    }
    return true;
}

std::wstring Version() {
    {
        std::lock_guard<std::mutex> g(s_lock);
        if (!s_version.empty()) return s_version;
    }
    std::wstring exe = Path();
    if (exe.empty()) return L"";
    std::string bytes;
    DWORD code = 0;
    RunCapture(exe, L"--version", 20000, [&](const std::string& b) { bytes += b; return true; }, &code);
    std::wstring v = Trim(Decode(bytes));
    std::lock_guard<std::mutex> g(s_lock);
    s_version = v;
    return v;
}

static bool ListCommand(const std::wstring& args, std::vector<wg::Row>& rows, std::wstring* error) {
    std::wstring out;
    DWORD code = 0;
    if (!Run(args, 4 * 60 * 1000, out, code, error)) return false;
    if (!wg::ParseTables(out, rows)) {
        // "No installed package found" and "No available upgrade found" are fine; anything else isn't
        bool empty = code == 0 || code == 0x8A150014 || IContains(out, L"No installed package") || IContains(out, L"No available upgrade") ||
                     IContains(out, L"No applicable");
        if (!empty) {
            std::vector<std::wstring> lines = wg::CleanLines(out);
            std::wstring last;
            for (auto& l : lines) if (!Trim(l).empty()) last = Trim(l);
            if (error) *error = last.empty() ? L"winget didn't return a list (code " + std::to_wstring(code) + L")." : last;
            Log(L"winget %ls: code 0x%08X, %ls", args.c_str(), code, last.c_str());
            return false;
        }
    }
    return true;
}

bool ListInstalled(std::vector<wg::Row>& rows, std::wstring* error) { return ListCommand(L"list", rows, error); }

bool ListUpgrades(bool includeUnknown, std::vector<wg::Row>& rows, std::wstring* error) {
    return ListCommand(includeUnknown ? L"upgrade --include-unknown" : L"upgrade", rows, error);
}

bool ExportIds(std::vector<std::pair<std::wstring, std::wstring>>& ids) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring file = std::wstring(tmp) + L"restock-export-" + std::to_wstring(GetCurrentProcessId()) + L".json";
    std::wstring out;
    DWORD code = 0;
    bool ok = Run(L"export -o " + QuoteArg(file) + L" --include-versions", 4 * 60 * 1000, out, code, nullptr);
    std::wstring text;
    if (ok) ok = ReadTextFile(file, text) && wg::ParseExport(text, ids);
    DeleteFileW(file.c_str());
    return ok;
}

UpgradeOutput Upgrade(const UpgradeRequest& req, const std::function<void(int, const std::wstring&)>& progress,
                      const std::atomic<bool>* cancel) {
    UpgradeOutput r;
    std::wstring exe = Path();
    if (exe.empty()) { r.output = L"winget isn't installed."; r.exitCode = 0x8A150000; return r; }
    std::wstring args = L"upgrade --id " + QuoteArg(req.id) + L" --exact --silent --accept-package-agreements";
    if (!req.source.empty()) args += L" --source " + QuoteArg(req.source);
    if (req.includeUnknown) args += L" --include-unknown";
    args += CommonArgs();
    Log(L"winget %ls%ls", args.c_str(), req.elevated ? L" (as administrator)" : L"");

    if (req.elevated) {
        // An elevated process can't hand us its output through a pipe, so cmd writes it to a file.
        std::wstring dir = DataDir() + L"\\run";
        EnsureDir(dir);
        std::wstring outFile = dir + L"\\winget-elevated.txt";
        DeleteFileW(outFile.c_str());
        std::wstring cmd = L"/s /c \"\"" + exe + L"\" " + args + L" > \"" + outFile + L"\" 2>&1\"";
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        if (progress) progress(-1, L"Installing as administrator…");
        DWORD code = 0;
        bool declined = false;
        r.started = RunElevatedAndWait(std::wstring(sys) + L"\\cmd.exe", cmd, 60 * 60 * 1000, &code, &declined);
        r.declined = declined;
        std::string bytes;
        ReadFileBytes(outFile, bytes);
        DeleteFileW(outFile.c_str());
        r.output = Decode(bytes);
        r.exitCode = code;
        return r;
    }

    std::string bytes;
    std::wstring lastPhase;
    int lastPct = -2;
    DWORD code = 0;
    r.started = RunCapture(exe, args, 60 * 60 * 1000, [&](const std::string& b) {
        if (cancel && *cancel) { r.cancelled = true; return false; }
        if (b.empty()) return true;
        bytes += b;
        if (progress) {
            // only the tail matters for progress: the current line, after the last redraw
            std::string tailBytes = bytes.size() > 4096 ? bytes.substr(bytes.size() - 4096) : bytes;
            std::wstring tail = Decode(tailBytes);
            size_t cut = tail.find_last_of(L"\r\n", tail.size() >= 2 ? tail.size() - 2 : 0);
            std::wstring cur = cut == std::wstring::npos ? tail : tail.substr(cut + 1);
            int pct = wg::ParseProgress(cur);
            std::wstring phase = wg::Phase(tail);
            if (pct != lastPct || phase != lastPhase) {
                lastPct = pct;
                lastPhase = phase;
                progress(pct, phase);
            }
        }
        return true;
    }, &code);
    r.output = Decode(bytes);
    r.exitCode = code;
    if (cancel && *cancel) r.cancelled = true;
    return r;
}

// ---------------------------------------------------------------- installing winget
static std::wstring Base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i < in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned char)in[i + 1] << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

static bool PowerShell(const std::wstring& script, DWORD timeoutMs, std::wstring* output) {
    // -EncodedCommand takes UTF-16LE base64, which sidesteps every quoting problem
    std::string utf16((const char*)script.data(), script.size() * sizeof(wchar_t));
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring ps = std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::string bytes;
    DWORD code = 1;
    bool ok = RunCapture(ps, L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + Base64(utf16), timeoutMs,
                         [&](const std::string& b) { bytes += b; return true; }, &code);
    if (output) *output = Decode(bytes);
    return ok && code == 0;
}

static std::wstring NativeArch() {
    USHORT process = 0, native = 0;
    using Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    if (auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"))
        if (fn(GetCurrentProcess(), &process, &native) && native == 0xAA64) return L"arm64";
    return L"x64";
}

bool Install(const std::function<void(const std::wstring&)>& status, std::wstring* error) {
    // 1. Windows 11 ships App Installer; on a new account it's sometimes just not registered yet
    status(L"Setting up winget…");
    PowerShell(L"Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe", 3 * 60 * 1000, nullptr);
    for (int i = 0; i < 10 && Path().empty(); ++i) Sleep(500);
    if (!Path().empty()) { Log(L"winget: registered App Installer"); return true; }

    // 2. download the newest release
    status(L"Downloading winget…");
    net::Response r;
    if (!net::Get(net::ApiBase() + L"/repos/microsoft/winget-cli/releases/latest", r, L"", true) || r.status != 200) {
        if (error) *error = r.error.empty() ? L"Couldn't reach GitHub to download winget (HTTP " + std::to_wstring(r.status) + L")." : r.error;
        return false;
    }
    json::Value rel;
    if (!json::Parser(Utf8ToWide(r.body)).Parse(rel)) { if (error) *error = L"GitHub sent an unexpected reply."; return false; }
    const json::Value* assets = rel.Get(L"assets");
    std::wstring bundleUrl, bundleHash, depsUrl, depsHash;
    if (assets && assets->type == json::Value::Array)
        for (auto& a : assets->arr) {
            std::wstring n = a.GetStr(L"name"), d = a.GetStr(L"digest");
            std::wstring h = IStartsWith(d, L"sha256:") ? Lower(d.substr(7)) : L"";
            if (IEquals(n, L"Microsoft.DesktopAppInstaller_8wekyb3d8bbwe.msixbundle")) { bundleUrl = a.GetStr(L"browser_download_url"); bundleHash = h; }
            if (IEquals(n, L"DesktopAppInstaller_Dependencies.zip")) { depsUrl = a.GetStr(L"browser_download_url"); depsHash = h; }
        }
    if (bundleUrl.empty()) { if (error) *error = L"The winget release has no installer."; return false; }
    std::wstring dir = DataDir() + L"\\downloads\\winget";
    EnsureDir(dir);
    std::wstring bundle = dir + L"\\AppInstaller.msixbundle", deps = dir + L"\\Dependencies.zip";
    std::wstring err;
    auto fetch = [&](const std::wstring& url, const std::wstring& path, const std::wstring& hash) {
        if (!net::Download(url, path, nullptr, &err)) return false;
        // GitHub's digest when there is one; the package's Microsoft signature is checked by Windows either way
        if (!hash.empty() && Sha256File(path) != hash) { err = L"The winget download didn't match its checksum."; DeleteFileW(path.c_str()); return false; }
        return true;
    };
    if (!fetch(bundleUrl, bundle, bundleHash) || (!depsUrl.empty() && !fetch(depsUrl, deps, depsHash))) {
        if (error) *error = err;
        return false;
    }
    // 3. install it with its dependencies (VCLibs, UI.Xaml) for this PC's architecture
    status(L"Installing winget…");
    std::wstring depDir = dir + L"\\deps";
    auto q = [](std::wstring v) {   // a PowerShell single-quoted string
        for (size_t i = 0; (i = v.find(L'\'', i)) != std::wstring::npos; i += 2) v.insert(i, 1, L'\'');
        return L"'" + v + L"'";
    };
    std::wstring script =
        L"$ErrorActionPreference='Stop';"
        L"$deps=@();"
        L"if (Test-Path -LiteralPath " + q(deps) + L") {"
        L"  Expand-Archive -LiteralPath " + q(deps) + L" -DestinationPath " + q(depDir) + L" -Force;"
        L"  $deps = Get-ChildItem -LiteralPath " + q(depDir) + L" -Recurse -Include *.appx,*.msix | Where-Object { $_.FullName -match '\\\\" + NativeArch() + L"\\\\' } | ForEach-Object { $_.FullName } };"
        L"Add-AppxPackage -Path " + q(bundle) + L" -DependencyPath $deps -ForceApplicationShutdown";
    std::wstring out;
    bool ok = PowerShell(script, 10 * 60 * 1000, &out);
    for (int i = 0; i < 20 && Path().empty(); ++i) Sleep(500);
    if (Path().empty()) {
        Log(L"winget install failed: %ls", out.c_str());
        if (error) *error = ok ? L"winget was installed but Windows hasn't made it available yet. Sign out and back in."
                               : L"Windows couldn't install winget. Install \"App Installer\" from the Microsoft Store.";
        return false;
    }
    Log(L"winget: installed from GitHub");
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_version.clear();
    }
    return true;
}

} // namespace winget
