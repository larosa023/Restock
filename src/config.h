// Restock - settings (DataDir()\settings.json)
#pragma once
#include "common.h"
#include <map>
#include <set>

struct Config {
    std::wstring theme = L"dark";       // a built-in theme id (dark, light, system, midnight…) or a custom theme's id
    std::wstring accent = L"theme";     // theme | windows | #RRGGBB
    int windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT, windowW = 0, windowH = 0;
    bool maximized = false;
    std::wstring page = L"updates";     // last page

    // schedule (run by the tray icon; there's no service)
    std::wstring schedule = L"daily";   // off | daily | weekly
    bool notify = true;                 // a notification when updates are waiting
    bool nightly = false;               // install updates automatically at night
    bool tray = true;                   // closing the window keeps Restock in the tray
    bool startup = true;                // start with Windows, in the tray
    long long lastCheck = 0;            // unix seconds of the last full check
    long long lastNightly = 0;          // unix seconds of the last overnight run
    std::wstring notified;              // the updates the last notification was about (so it isn't repeated)
    bool trayHintShown = false;

    // which apps get updated
    bool includeSelfUpdating = false;   // also update apps that update themselves
    bool includeUnknown = true;         // apps whose installed version winget can't tell
    bool reopen = true;                 // reopen apps Restock closed to update them
    std::set<std::wstring> ignored;     // package keys hidden entirely
    std::set<std::wstring> pinned;      // package keys never updated
    std::set<std::wstring> forceInclude;  // self-updating apps you chose to update with Restock anyway
    std::map<std::wstring, std::wstring> skipped;   // package key -> the version to skip

    // Restock's own updates
    bool selfBeta = false;
    bool selfAuto = true;               // install Restock updates by itself when it won't interrupt
    std::wstring skipVersion;           // Restock versions that failed to start or were rolled back (comma-separated)
};

extern Config g_cfg;
void ConfigLoad();
void ConfigSave();
bool IsSkippedVersion(const std::wstring& v);
void SkipVersion(const std::wstring& v);
void PruneSkippedVersions(const std::wstring& current);
