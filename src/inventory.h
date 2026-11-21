// Restock - what's installed: winget's view (IDs, sources, available versions) merged with the
// Apps & features entries in the registry (names, publishers, icons, install folders), plus the
// larosa023 suite filter and running-process checks.
#pragma once
#include "common.h"
#include "json.h"

struct Package {
    std::wstring key;               // stable identity: the winget ID, or "arp:<root>\<subkey>" for apps winget doesn't know
    std::wstring id;                // winget ID ("" when winget can't update the app)
    std::wstring name, publisher, version, available, source;   // source: winget | msstore | <custom>; "" for registry-only
    std::wstring location;          // install folder
    std::wstring iconPath;
    int iconIndex = 0;
    std::wstring runDir;            // the folder its processes run from ("" when it can't be told safely)
    std::wstring url;               // the publisher's page, for apps winget doesn't know
    bool machine = false;           // installed for everyone (usually needs administrator rights to update)
    bool squirrel = false;          // a Squirrel/Electron install that updates itself
    bool truncatedId = false;       // winget shortened the ID and it couldn't be completed
};

struct Inventory {
    std::vector<Package> pkgs;
    bool wingetOk = false;          // winget ran and listed apps
    bool wingetMissing = false;
    std::wstring wingetError;
    std::wstring wingetVersion;
    long long scannedAt = 0;        // unix seconds
};

// Runs winget and reads the registry. Takes several seconds; call it on a worker thread.
Inventory ScanInventory(bool includeUnknown);

// Cached results, so the window opens instantly with what was found last time
bool SaveInventoryCache(const Inventory& inv);
bool LoadInventoryCache(Inventory& inv);

// Apps that keep themselves up to date (browsers, launchers, chat apps, Microsoft Store apps…)
bool IsSelfUpdating(const Package& p);

// The larosa023 suite, from Hangar's manifest: those apps are Hangar's job
class Suite {
public:
    bool Load(const std::wstring& manifestText);
    bool Contains(const Package& p) const;
    bool Empty() const { return apps_.empty(); }
private:
    struct App { std::wstring name, registry, packId, dir, exe; };
    std::vector<App> apps_;
};

// Icon pixels (premultiplied BGRA) from an exe, dll or ico
bool ExtractIconPixels(const std::wstring& path, int index, UINT px, UINT& w, UINT& h, std::vector<uint8_t>& pixels);

// For each folder, the processes running from it
std::vector<std::vector<DWORD>> FindRunningIn(const std::vector<std::wstring>& dirs);
// The exe of the process (among pids) that owns a visible window; else the first one's exe
std::wstring MainExeOf(const std::vector<DWORD>& pids);
std::wstring ProcessPath(DWORD pid);

// A folder is safe to match processes against only if it's an app's own folder (not Program Files itself)
bool IsAppFolder(const std::wstring& dir);
