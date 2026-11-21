// Restock - one-click updates for every app that isn't part of the larosa023 suite
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#ifndef RESTOCK_VERSION
#define RESTOCK_VERSION L"0.1.0"
#endif
#define RESTOCK_REPO_OWNER L"larosa023"
#define RESTOCK_REPO_NAME L"Restock"
// Hangar's catalog of the suite: every app listed there is Hangar's job, so Restock leaves it alone
#define HANGAR_MANIFEST_URL L"https://raw.githubusercontent.com/larosa023/Hangar/main/manifest.json"

// Messages posted from worker threads to the main window. lParam owns a heap object
// that the window procedure takes over and deletes.
enum : UINT {
    WM_RESTOCK_SCAN = WM_APP + 1,      // lParam: ScanResult*
    WM_RESTOCK_ICON,                   // lParam: IconResult*
    WM_RESTOCK_PROCS,                  // lParam: ProcResult*
    WM_RESTOCK_PROGRESS,               // lParam: UpdateProgress*
    WM_RESTOCK_DONE,                   // lParam: UpdateDone*
    WM_RESTOCK_QUEUE_IDLE,             // the update queue finished (wParam: number of updates it ran)
    WM_RESTOCK_SUITE,                  // lParam: SuiteResult*
    WM_RESTOCK_ACTIVATE,               // another Restock.exe asked us to come to the front
    WM_RESTOCK_SELF,                   // lParam: SelfUpdateState* (Restock's own updater)
    WM_RESTOCK_TRAY,                   // tray icon callback
    WM_RESTOCK_QUIT,                   // Restock.exe --quit: exit for real (Hangar uses this before updating Restock)
    WM_RESTOCK_WINGET,                 // lParam: WingetSetupResult*
    WM_RESTOCK_ASK,                    // lParam: AskRequest* (the queue needs a yes/no from you)
};
