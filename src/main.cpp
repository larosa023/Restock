// Restock - one-click updates for every app on your PC that isn't part of the larosa023 suite
//
//   Restock.exe              open Restock (or bring the running one to the front)
//   Restock.exe --tray       start in the tray without a window (Start with Windows uses this)
//   Restock.exe --quit       ask a running Restock to exit (Hangar uses this before updating it)
//   Restock.exe --version    exit with code 0 (used by smoke tests)
#include "common.h"
#include "ui.h"
#include "util.h"
#include "config.h"
#include "net.h"
#include "selfupdate.h"
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commctrl.h>

static HANDLE g_mutex;
static const wchar_t* kMutexName = L"Local\\Restock.larosa023.SingleInstance";
static const wchar_t* kWindowClass = L"Restock.larosa023.Main";

void ReleaseInstanceMutex() {
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
}
void ReacquireInstanceMutex() {
    if (!g_mutex) g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
}

static bool HasArg(const wchar_t* cmdLine, const wchar_t* arg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"x", &argc);
    bool hit = false;
    for (int i = 0; argv && i < argc; ++i) hit |= _wcsicmp(argv[i], arg) == 0;
    if (argv) LocalFree(argv);
    return hit;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    const wchar_t* cmd = GetCommandLineW();
    if (HasArg(cmd, L"--version")) return 0;

    if (HasArg(cmd, L"--quit")) {
        // close the running Restock for real (not just to the tray) and wait for it to go
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
            PostMessageW(existing, WM_RESTOCK_QUIT, 0, 0);
            if (h) { WaitForSingleObject(h, 15000); CloseHandle(h); }
        }
        return 0;
    }

    bool tray = HasArg(cmd, L"--tray");
    selfupdate::AfterUpdate after = selfupdate::ParseArgs(cmd);
    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS && after.active) {
        // the copy we're replacing is letting go; give it a moment
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        for (int i = 0; i < 50 && !g_mutex; ++i) {
            Sleep(100);
            HANDLE m = CreateMutexW(nullptr, TRUE, kMutexName);
            if (GetLastError() != ERROR_ALREADY_EXISTS) g_mutex = m; else CloseHandle(m);
        }
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // already running: show it (unless this was just the sign-in start)
        if (!tray) {
            if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existing, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(existing, WM_RESTOCK_ACTIVATE, 0, 0);
            }
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetCurrentProcessExplicitAppUserModelID(L"larosa023.Restock");
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    LogInit();
    Log(L"==== Restock %ls starting (data: %ls%ls%ls)", RESTOCK_VERSION, DataDir().c_str(), IsPortable() ? L", portable" : L"", tray ? L", tray" : L"");
    ConfigLoad();
    net::Init();

    HWND hwnd = UiCreate(inst, show, after, tray);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Restock couldn't start. Details are in restock.log.", L"Restock", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    net::Shutdown();
    Log(L"Restock exiting");
    CoUninitialize();
    if (g_mutex) CloseHandle(g_mutex);
    return 0;
}
