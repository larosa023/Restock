// Restock - what's installed (see inventory.h)
#include "inventory.h"
#include "winget.h"
#include "util.h"
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <map>
#include <set>
#include <mutex>
#include <ctime>

// ---------------------------------------------------------------- Apps & features
static std::wstring RegStr(HKEY key, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, (BYTE*)buf, &size) != ERROR_SUCCESS) return L"";
    if (type != REG_SZ && type != REG_EXPAND_SZ) return L"";
    size_t n = size / sizeof(wchar_t);
    while (n && buf[n - 1] == 0) --n;
    std::wstring s(buf, n);
    return Trim(type == REG_EXPAND_SZ ? ExpandEnv(s) : s);
}

static DWORD RegDword(HKEY key, const wchar_t* name) {
    DWORD v = 0, size = sizeof(v), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, (BYTE*)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) return 0;
    return v;
}

struct ArpEntry {
    std::wstring key, name, version, publisher, location, icon, uninstall, url;
    bool machine = false;
};

static const wchar_t* kUninstallRoot = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

static std::vector<ArpEntry> ReadArp() {
    std::vector<ArpEntry> out;
    struct Root { HKEY h; REGSAM view; bool machine; const wchar_t* tag; } roots[] = {
        {HKEY_CURRENT_USER, 0, false, L"HKCU"},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, true, L"HKLM64"},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, true, L"HKLM32"},
    };
    for (auto& r : roots) {
        HKEY k;
        if (RegOpenKeyExW(r.h, kUninstallRoot, 0, KEY_READ | r.view, &k) != ERROR_SUCCESS) continue;
        wchar_t sub[512];
        for (DWORD i = 0;; ++i) {
            DWORD len = 512;
            if (RegEnumKeyExW(k, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY e;
            if (RegOpenKeyExW(k, sub, 0, KEY_READ | r.view, &e) != ERROR_SUCCESS) continue;
            ArpEntry a;
            a.key = std::wstring(r.tag) + L"\\" + sub;
            a.machine = r.machine;
            a.name = RegStr(e, L"DisplayName");
            a.version = RegStr(e, L"DisplayVersion");
            a.publisher = RegStr(e, L"Publisher");
            a.location = RegStr(e, L"InstallLocation");
            a.icon = RegStr(e, L"DisplayIcon");
            a.uninstall = RegStr(e, L"UninstallString");
            a.url = RegStr(e, L"URLInfoAbout");
            if (a.url.empty()) a.url = RegStr(e, L"HelpLink");
            bool hidden = RegDword(e, L"SystemComponent") == 1 || !RegStr(e, L"ParentKeyName").empty();
            std::wstring rt = RegStr(e, L"ReleaseType");
            bool patch = IEquals(rt, L"Update") || IEquals(rt, L"Hotfix") || IEquals(rt, L"Security Update") || IEquals(rt, L"ServicePack");
            RegCloseKey(e);
            if (a.name.empty() || hidden || patch) continue;
            // the same app listed under both registry views
            bool dup = false;
            for (auto& o : out) dup |= IEquals(o.name, a.name) && o.version == a.version && o.machine == a.machine;
            if (!dup) out.push_back(a);
        }
        RegCloseKey(k);
    }
    return out;
}

static void IconFromDisplayIcon(const std::wstring& icon0, std::wstring& path, int& index) {
    std::wstring icon = Trim(icon0);
    index = 0;
    if (!icon.empty() && icon[0] == L'"') {
        size_t e = icon.find(L'"', 1);
        std::wstring rest = e == std::wstring::npos ? L"" : icon.substr(e + 1);
        icon = icon.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
        size_t comma = rest.find(L',');
        if (comma != std::wstring::npos) index = _wtoi(rest.c_str() + comma + 1);
    } else {
        size_t comma = icon.rfind(L',');
        if (comma != std::wstring::npos && comma > 2) {
            std::wstring n = Trim(icon.substr(comma + 1));
            if (!n.empty() && (iswdigit(n[0]) || n[0] == L'-')) { index = _wtoi(n.c_str()); icon = icon.substr(0, comma); }
        }
    }
    path = FileExists(icon) ? icon : L"";
}

bool IsAppFolder(const std::wstring& dir0) {
    std::wstring dir = Trim(dir0);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.size() < 4 || !DirExists(dir)) return false;
    static std::vector<std::wstring> generic;
    if (generic.empty()) {
        for (const GUID* g : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_LocalAppData, &FOLDERID_RoamingAppData,
                              &FOLDERID_Profile, &FOLDERID_Windows, &FOLDERID_UserProgramFiles, &FOLDERID_ProgramData, &FOLDERID_Desktop,
                              &FOLDERID_Downloads, &FOLDERID_Documents})
            generic.push_back(KnownFolder(*g));
        generic.push_back(KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft");
        generic.push_back(KnownFolder(FOLDERID_LocalAppData) + L"\\Temp");
    }
    for (auto& g : generic) if (!g.empty() && IEquals(dir, g)) return false;
    // nothing inside Windows itself, and not a drive's top folders
    std::wstring win = KnownFolder(FOLDERID_Windows);
    if (!win.empty() && PathStartsWith(dir, win)) return false;
    size_t parts = 0;
    for (wchar_t c : dir) parts += c == L'\\';
    return parts >= 2;
}

static std::wstring FindExeIn(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*.exe").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::wstring hit;
    do {
        std::wstring n = Lower(fd.cFileName);
        if (n.find(L"unins") != std::wstring::npos || n.find(L"setup") != std::wstring::npos || n == L"update.exe" ||
            n.find(L"crash") != std::wstring::npos || n.find(L"helper") != std::wstring::npos) continue;
        hit = dir + L"\\" + fd.cFileName;
        break;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return hit;
}

static void FillFromArp(Package& p, const ArpEntry& a) {
    p.publisher = a.publisher;
    p.location = a.location;
    p.machine = a.machine;
    p.url = a.url;
    IconFromDisplayIcon(a.icon, p.iconPath, p.iconIndex);
    std::wstring dir = a.location;
    if (!IsAppFolder(dir) && !p.iconPath.empty() && IContains(p.iconPath, L".exe")) dir = PathParent(p.iconPath);
    if (IsAppFolder(dir)) p.runDir = dir;
    if (p.iconPath.empty() && IsAppFolder(a.location)) p.iconPath = FindExeIn(a.location);
    // Squirrel installs keep Update.exe beside the app and update themselves
    p.squirrel = IContains(a.uninstall, L"\\Update.exe\" --uninstall") || IContains(a.uninstall, L"\\Update.exe --uninstall") ||
                 (!a.location.empty() && PathStartsWith(a.location, KnownFolder(FOLDERID_LocalAppData)) && FileExists(a.location + L"\\Update.exe"));
}

// ---------------------------------------------------------------- scan
static int MatchArp(const std::vector<ArpEntry>& arp, const std::vector<bool>& used, const std::wstring& name, const std::wstring& version) {
    bool shortened = wg::IsTruncated(name);
    std::wstring prefix = name;
    if (shortened) prefix.pop_back();
    int best = -1, bestScore = -1;
    for (size_t i = 0; i < arp.size(); ++i) {
        bool nameOk = shortened ? IStartsWith(arp[i].name, prefix) : IEquals(arp[i].name, name);
        if (!nameOk) continue;
        int score = (used[i] ? 0 : 2) + (arp[i].version == version ? 1 : 0);
        if (score > bestScore) { best = (int)i; bestScore = score; }
    }
    return best;
}

Inventory ScanInventory(bool includeUnknown) {
    Inventory inv;
    inv.scannedAt = (long long)time(nullptr);
    std::vector<ArpEntry> arp = ReadArp();
    std::vector<bool> used(arp.size(), false);
    std::set<std::wstring> keys;
    auto add = [&](Package p) {
        std::wstring k = Lower(p.key);
        if (keys.count(k)) return;
        keys.insert(k);
        inv.pkgs.push_back(std::move(p));
    };

    if (winget::Path().empty()) {
        inv.wingetMissing = true;
        inv.wingetError = L"winget isn't installed.";
    } else {
        inv.wingetVersion = winget::Version();
        std::vector<wg::Row> rows, ups;
        std::wstring err;
        DWORD t0 = GetTickCount();
        inv.wingetOk = winget::ListInstalled(rows, &err);
        if (!inv.wingetOk) inv.wingetError = err;
        else if (!winget::ListUpgrades(includeUnknown, ups, &err)) Log(L"winget upgrade: %ls", err.c_str());
        Log(L"winget: %zu installed, %zu with updates (%lu ms)", rows.size(), ups.size(), GetTickCount() - t0);

        // complete any IDs winget shortened (older versions do that when the output is redirected)
        bool shortened = false;
        for (auto& r : rows) shortened |= wg::IsTruncated(r.id);
        for (auto& r : ups) shortened |= wg::IsTruncated(r.id);
        std::vector<std::wstring> full;
        if (shortened) {
            std::vector<std::pair<std::wstring, std::wstring>> ex;
            if (winget::ExportIds(ex)) for (auto& e : ex) full.push_back(e.first);
        }
        auto resolve = [&](wg::Row& r) -> bool {
            if (!wg::IsTruncated(r.id)) return true;
            std::wstring f = wg::ResolveTruncated(r.id, full);
            if (f.empty()) return false;
            r.id = f;
            return true;
        };
        std::map<std::wstring, wg::Row> upById;
        for (auto& u : ups) { if (resolve(u)) upById[Lower(u.id)] = u; }

        for (auto& r : rows) {
            bool arpOnly = IStartsWith(r.id, L"ARP\\");
            bool msix = IStartsWith(r.id, L"MSIX\\");
            if (msix && r.source.empty()) continue;     // Windows' own packaged components
            Package p;
            bool ok = resolve(r);
            int idx = MatchArp(arp, used, r.name, r.version);
            if (arpOnly) {
                if (idx < 0) continue;                   // shown below from the registry
                p.key = L"arp:" + arp[idx].key;
                p.name = arp[idx].name;
                p.version = arp[idx].version.empty() ? r.version : arp[idx].version;
            } else {
                p.id = r.id;
                p.truncatedId = !ok;
                p.key = r.id;
                p.name = r.name;
                p.version = r.version;
                p.source = r.source.empty() ? L"winget" : r.source;
                auto u = upById.find(Lower(r.id));
                p.available = u != upById.end() ? u->second.available : r.available;
                if (u != upById.end()) upById.erase(u);
                if (idx >= 0 && wg::IsTruncated(p.name)) p.name = arp[idx].name;
            }
            if (idx >= 0) { FillFromArp(p, arp[idx]); used[idx] = true; }
            add(std::move(p));
        }
        // updates for packages `list` didn't show (rare: a different name or scope)
        for (auto& kv : upById) {
            Package p;
            p.id = p.key = kv.second.id;
            p.name = kv.second.name;
            p.version = kv.second.version;
            p.available = kv.second.available;
            p.source = kv.second.source.empty() ? L"winget" : kv.second.source;
            int idx = MatchArp(arp, used, p.name, p.version);
            if (idx >= 0) { FillFromArp(p, arp[idx]); used[idx] = true; }
            add(std::move(p));
        }
    }
    // everything in Apps & features that winget didn't account for
    for (size_t i = 0; i < arp.size(); ++i) {
        if (used[i]) continue;
        bool shown = false;
        for (auto& p : inv.pkgs) shown |= IEquals(p.name, arp[i].name);
        if (shown) continue;
        Package p;
        p.key = L"arp:" + arp[i].key;
        p.name = arp[i].name;
        p.version = arp[i].version;
        FillFromArp(p, arp[i]);
        add(std::move(p));
    }
    std::sort(inv.pkgs.begin(), inv.pkgs.end(), [](const Package& a, const Package& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return inv;
}

// ---------------------------------------------------------------- cache
static std::wstring CachePath() { return CacheDir() + L"\\inventory.json"; }

bool SaveInventoryCache(const Inventory& inv) {
    json::Value root = json::Value::MakeObject();
    root.Set(L"schema", json::Value::Num(1));
    root.Set(L"scannedAt", json::Value::Num((double)inv.scannedAt));
    root.Set(L"wingetOk", json::Value::Boolean(inv.wingetOk));
    root.Set(L"wingetVersion", json::Value::Str(inv.wingetVersion));
    json::Value arr = json::Value::MakeArray();
    for (auto& p : inv.pkgs) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"key", json::Value::Str(p.key));
        o.Set(L"id", json::Value::Str(p.id));
        o.Set(L"name", json::Value::Str(p.name));
        o.Set(L"publisher", json::Value::Str(p.publisher));
        o.Set(L"version", json::Value::Str(p.version));
        o.Set(L"available", json::Value::Str(p.available));
        o.Set(L"source", json::Value::Str(p.source));
        o.Set(L"location", json::Value::Str(p.location));
        o.Set(L"icon", json::Value::Str(p.iconPath));
        o.Set(L"iconIndex", json::Value::Num(p.iconIndex));
        o.Set(L"runDir", json::Value::Str(p.runDir));
        o.Set(L"url", json::Value::Str(p.url));
        o.Set(L"machine", json::Value::Boolean(p.machine));
        o.Set(L"squirrel", json::Value::Boolean(p.squirrel));
        o.Set(L"truncatedId", json::Value::Boolean(p.truncatedId));
        arr.arr.push_back(std::move(o));
    }
    root.Set(L"packages", std::move(arr));
    std::wstring text;
    json::Write(root, text);
    return WriteTextFileAtomic(CachePath(), text);
}

bool LoadInventoryCache(Inventory& inv) {
    std::wstring text;
    if (!ReadTextFile(CachePath(), text)) return false;
    json::Value root;
    if (!json::Parser(text).Parse(root) || root.type != json::Value::Object) return false;
    inv = Inventory{};
    inv.scannedAt = (long long)root.GetNum(L"scannedAt", 0);
    inv.wingetOk = root.GetBool(L"wingetOk", false);
    inv.wingetVersion = root.GetStr(L"wingetVersion");
    const json::Value* arr = root.Get(L"packages");
    if (!arr || arr->type != json::Value::Array) return false;
    for (auto& o : arr->arr) {
        Package p;
        p.key = o.GetStr(L"key");
        if (p.key.empty()) continue;
        p.id = o.GetStr(L"id");
        p.name = o.GetStr(L"name");
        p.publisher = o.GetStr(L"publisher");
        p.version = o.GetStr(L"version");
        p.available = o.GetStr(L"available");
        p.source = o.GetStr(L"source");
        p.location = o.GetStr(L"location");
        p.iconPath = o.GetStr(L"icon");
        p.iconIndex = (int)o.GetNum(L"iconIndex", 0);
        p.runDir = o.GetStr(L"runDir");
        p.url = o.GetStr(L"url");
        p.machine = o.GetBool(L"machine", false);
        p.squirrel = o.GetBool(L"squirrel", false);
        p.truncatedId = o.GetBool(L"truncatedId", false);
        inv.pkgs.push_back(std::move(p));
    }
    return true;
}

// ---------------------------------------------------------------- self-updating apps
bool IsSelfUpdating(const Package& p) {
    if (IEquals(p.source, L"msstore")) return true;    // the Microsoft Store keeps these current
    if (p.squirrel) return true;                       // Squirrel installs (Discord, Slack, Teams classic…)
    static const wchar_t* ids[] = {
        L"Google.Chrome", L"Microsoft.Edge", L"Microsoft.EdgeWebView2Runtime", L"Mozilla.Firefox", L"Mozilla.Thunderbird",
        L"Brave.Brave", L"Opera.Opera", L"Vivaldi.Vivaldi", L"TheBrowserCompany.Arc", L"Discord.Discord", L"Valve.Steam",
        L"EpicGames.EpicGamesLauncher", L"Blizzard.BattleNet", L"ElectronicArts.EADesktop", L"ElectronicArts.Origin",
        L"Ubisoft.Connect", L"GOG.Galaxy", L"RiotGames.", L"RockstarGames.", L"Amazon.Games", L"Spotify.Spotify",
        L"SlackTechnologies.Slack", L"Microsoft.Teams", L"Zoom.Zoom", L"Microsoft.OneDrive", L"Microsoft.Office", L"Microsoft.VisualStudioCode",
        L"Anysphere.Cursor", L"Microsoft.PowerToys", L"Dropbox.Dropbox", L"Google.GoogleDrive", L"Google.Drive", L"Telegram.TelegramDesktop",
        L"OpenWhisperSystems.Signal", L"Notion.Notion", L"Figma.Figma", L"Obsidian.Obsidian", L"Postman.Postman", L"GitHub.GitHubDesktop",
        L"Logitech.GHUB", L"Nvidia.GeForceExperience", L"Nvidia.App", L"Razer.Synapse", L"Corsair.iCUE", L"SteelSeries.GG",
        L"JetBrains.Toolbox", L"Docker.DockerDesktop", L"Adobe.CreativeCloud", L"Adobe.Acrobat", L"Overwolf.", L"Plex.Plex",
        L"AgileBits.1Password", L"Bitwarden.Bitwarden", L"Parsec.Parsec", L"AnyDeskSoftwareGmbH.AnyDesk", L"TeamViewer.TeamViewer",
        L"Malwarebytes.Malwarebytes", L"NordSecurity.NordVPN", L"ExpressVPN.ExpressVPN", L"Proton.ProtonVPN", L"Mattermost.MattermostDesktop",
        L"Microsoft.Skype", L"Loom.Loom", L"Linear.Linear", L"Beeper.Beeper", L"Microsoft.WindowsApp", L"Microsoft.CompanyPortal",
    };
    for (auto* id : ids) {
        std::wstring s = id;
        if (s.back() == L'.') { if (IStartsWith(p.id, s)) return true; }
        else if (IEquals(p.id, s) || IStartsWith(p.id, s + L".")) return true;
    }
    if (p.id.empty()) {
        static const wchar_t* names[] = {L"Google Chrome", L"Microsoft Edge", L"Mozilla Firefox", L"Steam", L"Discord", L"Epic Games Launcher",
                                         L"Battle.net", L"EA app", L"Ubisoft Connect", L"Spotify", L"Zoom", L"Microsoft OneDrive", L"Brave",
                                         L"Opera", L"Vivaldi", L"Slack", L"Microsoft Teams", L"Microsoft Visual Studio Code"};
        for (auto* n : names) if (IEquals(p.name, n) || IStartsWith(p.name, std::wstring(n) + L" ")) return true;
    }
    return false;
}

// ---------------------------------------------------------------- the larosa023 suite
bool Suite::Load(const std::wstring& text) {
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) return false;
    const json::Value* apps = v.Get(L"apps");
    if (!apps || apps->type != json::Value::Array) return false;
    std::vector<App> out;
    for (auto& a : apps->arr) {
        App x;
        x.name = a.GetStr(L"name");
        x.exe = a.GetStr(L"exe");
        if (const json::Value* in = a.Get(L"install")) {
            x.registry = in->GetStr(L"registry");
            x.packId = in->GetStr(L"packId");
            x.dir = ExpandEnv(in->GetStr(L"dir"));
        }
        if (!x.name.empty()) out.push_back(x);
    }
    // the hub itself, and Restock
    out.push_back({L"Hangar", L"", L"", ExpandEnv(L"%LOCALAPPDATA%\\Programs\\Hangar"), L"Hangar.exe"});
    out.push_back({L"Restock", L"", L"", ExpandEnv(L"%LOCALAPPDATA%\\Programs\\Restock"), L"Restock.exe"});
    apps_ = std::move(out);
    return true;
}

static bool NameMatches(const std::wstring& name, const std::wstring& want) {
    if (want.empty()) return false;
    if (IEquals(name, want)) return true;
    // electron-builder adds the version: "Amplite 1.15.0"
    return IStartsWith(name, want + L" ") && name.size() > want.size() + 1 &&
           (iswdigit(name[want.size() + 1]) || name[want.size() + 1] == L'v');
}

bool Suite::Contains(const Package& p) const {
    if (IStartsWith(p.id, L"larosa023.") || IContains(p.publisher, L"larosa")) return true;
    std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    for (auto& a : apps_) {
        // a name alone isn't proof: another company's "Prism" or "Drift" is a different app. The suite's
        // installers leave the publisher empty, name it after the app (Tauri, Velopack) or after you.
        bool ourPublisher = p.publisher.empty() || IEquals(p.publisher, a.name) || (!a.registry.empty() && IEquals(p.publisher, a.registry)) ||
                            (!a.packId.empty() && IEquals(p.publisher, a.packId));
        if (ourPublisher && (NameMatches(p.name, a.name) || NameMatches(p.name, a.registry))) return true;
        if (!a.dir.empty() && (PathStartsWith(p.location, a.dir) || PathStartsWith(p.runDir, a.dir))) return true;
        if (!a.packId.empty() && (PathStartsWith(p.location, local + L"\\" + a.packId) || PathStartsWith(p.runDir, local + L"\\" + a.packId))) return true;
    }
    return false;
}

// ---------------------------------------------------------------- icons
bool ExtractIconPixels(const std::wstring& path, int index, UINT px, UINT& w, UINT& h, std::vector<uint8_t>& pixels) {
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(path.c_str(), index, 0, &icon, nullptr, MAKELONG(px, 16))) || !icon) {
        if (PrivateExtractIconsW(path.c_str(), index, (int)px, (int)px, &icon, nullptr, 1, 0) != 1 || !icon) return false;
    }
    ICONINFO ii{};
    bool ok = false;
    if (GetIconInfo(icon, &ii)) {
        BITMAP bm{};
        GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
        w = (UINT)bm.bmWidth;
        h = (UINT)(ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2);
        if (w && h && w <= 512 && h <= 512) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = (LONG)w;
            bi.bmiHeader.biHeight = -(LONG)h;    // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            pixels.assign((size_t)w * h * 4, 0);
            HDC dc = GetDC(nullptr);
            if (ii.hbmColor && GetDIBits(dc, ii.hbmColor, 0, h, pixels.data(), &bi, DIB_RGB_COLORS)) {
                bool anyAlpha = false;
                for (size_t i = 3; i < pixels.size(); i += 4) if (pixels[i]) { anyAlpha = true; break; }
                if (!anyAlpha) {
                    // an old icon without alpha: the mask says which pixels show
                    std::vector<uint8_t> mask((size_t)w * h * 4);
                    if (GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS))
                        for (size_t i = 0; i < pixels.size(); i += 4) pixels[i + 3] = mask[i] ? 0 : 255;
                    else
                        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
                }
                // premultiply for Direct2D
                for (size_t i = 0; i < pixels.size(); i += 4) {
                    unsigned a = pixels[i + 3];
                    pixels[i] = (uint8_t)(pixels[i] * a / 255);
                    pixels[i + 1] = (uint8_t)(pixels[i + 1] * a / 255);
                    pixels[i + 2] = (uint8_t)(pixels[i + 2] * a / 255);
                }
                ok = true;
            }
            ReleaseDC(nullptr, dc);
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }
    DestroyIcon(icon);
    return ok;
}

// ---------------------------------------------------------------- processes
std::wstring ProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t path[MAX_PATH * 2];
    DWORD len = MAX_PATH * 2;
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) r.assign(path, len);
    CloseHandle(h);
    return r;
}

// pid -> image path; a process's path never changes, so polls only open new processes
static std::mutex s_procLock;
static std::map<DWORD, std::pair<std::wstring, std::wstring>> s_procCache;   // pid -> (exe name, full path)

std::vector<std::vector<DWORD>> FindRunningIn(const std::vector<std::wstring>& dirs) {
    std::vector<std::vector<DWORD>> result(dirs.size());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    std::lock_guard<std::mutex> g(s_procLock);
    std::map<DWORD, std::pair<std::wstring, std::wstring>> next;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID <= 4) continue;
        std::wstring path;
        auto it = s_procCache.find(pe.th32ProcessID);
        if (it != s_procCache.end() && it->second.first == pe.szExeFile) path = it->second.second;
        else path = ProcessPath(pe.th32ProcessID);
        next[pe.th32ProcessID] = {pe.szExeFile, path};
        if (path.empty()) continue;
        for (size_t i = 0; i < dirs.size(); ++i)
            if (!dirs[i].empty() && PathStartsWith(path, dirs[i])) result[i].push_back(pe.th32ProcessID);
    }
    s_procCache = std::move(next);
    CloseHandle(snap);
    return result;
}

struct WinFind { std::set<DWORD> pids; DWORD hit = 0; };
static BOOL CALLBACK FindVisible(HWND hwnd, LPARAM lp) {
    auto* f = (WinFind*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (f->pids.count(pid) && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER) && GetWindowTextLengthW(hwnd) > 0) { f->hit = pid; return FALSE; }
    return TRUE;
}

std::wstring MainExeOf(const std::vector<DWORD>& pids) {
    if (pids.empty()) return L"";
    WinFind f;
    f.pids.insert(pids.begin(), pids.end());
    EnumWindows(FindVisible, (LPARAM)&f);
    std::wstring p = ProcessPath(f.hit ? f.hit : pids[0]);
    return p;
}
