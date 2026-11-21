// Restock - the tray icon
#include "tray.h"
#include "util.h"
#include <shellapi.h>
#include <commctrl.h>

namespace tray {

static NOTIFYICONDATAW s_nid;
static bool s_shown = false;
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

UINT TaskbarCreatedMessage() {
    static UINT m = RegisterWindowMessageW(L"TaskbarCreated");
    return m;
}

void Add(HWND hwnd, const std::wstring& tip) {
    if (s_shown) Shell_NotifyIconW(NIM_DELETE, &s_nid);
    s_nid = NOTIFYICONDATAW{};
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = hwnd;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    s_nid.uCallbackMessage = WM_RESTOCK_TRAY;
    if (FAILED(LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), LIM_SMALL, &s_nid.hIcon)))
        s_nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcsncpy_s(s_nid.szTip, tip.c_str(), _TRUNCATE);
    s_shown = Shell_NotifyIconW(NIM_ADD, &s_nid) != FALSE;
    s_nid.uVersion = NOTIFYICON_VERSION_4;
    if (s_shown) Shell_NotifyIconW(NIM_SETVERSION, &s_nid);
}

void SetTip(const std::wstring& tip) {
    if (!s_shown) return;
    wcsncpy_s(s_nid.szTip, tip.c_str(), _TRUNCATE);
    s_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &s_nid);
}

void Remove() {
    if (!s_shown) return;
    Shell_NotifyIconW(NIM_DELETE, &s_nid);
    if (s_nid.hIcon) DestroyIcon(s_nid.hIcon);
    s_nid.hIcon = nullptr;
    s_shown = false;
}

bool Shown() { return s_shown; }

void Notify(const std::wstring& title, const std::wstring& text) {
    if (!s_shown) return;
    s_nid.uFlags = NIF_INFO;
    wcsncpy_s(s_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(s_nid.szInfo, text.c_str(), _TRUNCATE);
    s_nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    s_nid.hBalloonIcon = nullptr;
    Shell_NotifyIconW(NIM_MODIFY, &s_nid);
}

UINT Menu(HWND hwnd, int updates, bool busy) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CmdOpen, L"Open Restock");
    SetMenuDefaultItem(m, CmdOpen, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (busy ? MF_GRAYED : 0), CmdCheck, L"Check for updates now");
    std::wstring all = updates > 0 ? L"Update all (" + std::to_wstring(updates) + L")" : L"Update all";
    AppendMenuW(m, MF_STRING | (updates > 0 && !busy ? 0 : MF_GRAYED), CmdUpdateAll, all.c_str());
    AppendMenuW(m, MF_STRING, CmdHistory, L"History");
    AppendMenuW(m, MF_STRING, CmdSettings, L"Settings");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CmdQuit, L"Quit Restock");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);   // so the menu closes when you click elsewhere
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    return cmd;
}

void SetStartWithWindows(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\" --tray";
        RegSetValueExW(k, L"Restock", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"Restock");
    }
    RegCloseKey(k);
}

bool StartsWithWindows() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024];
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    bool ok = RegQueryValueExW(k, L"Restock", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(k);
    if (!ok) return false;
    buf[size / sizeof(wchar_t)] = 0;
    return IContains(buf, ExePath());
}

} // namespace tray
