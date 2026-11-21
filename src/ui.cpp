// Restock - main window: updates, all apps, history, themes, settings. Drawn with Direct2D; all
// state lives on this thread and background jobs report back with posted messages.
#include "ui.h"
#include "gfx.h"
#include "inventory.h"
#include "jobs.h"
#include "config.h"
#include "util.h"
#include "net.h"
#include "selfupdate.h"
#include "history.h"
#include "json.h"
#include "themes.h"
#include "tray.h"
#include "winget.h"
#include <windowsx.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <set>
#include <memory>

using namespace gfx;

// ---------------------------------------------------------------- state
struct PkgState {
    Package p;
    bool suite = false;                 // part of the larosa023 suite: Hangar's job
    bool selfUpdating = false;
    std::vector<DWORD> pids;            // running from its folder
    // the update queue
    bool queued = false;
    QueueState qstate = QueueState::Waiting;
    int percent = -1;
    float shownPercent = 0;
    std::wstring opText;
    // how its last update in this session went
    enum Result { None, Ok, Restart, Failed, Skipped } result = None;
    std::wstring resultText;
    // icon
    bool iconRequested = false;
    std::vector<uint8_t> iconPixels;    // decoded, freed once it's on the GPU
    UINT iconW = 0, iconH = 0;
    ID2D1Bitmap* icon = nullptr;
    float hover = 0;
};

enum class Page { Updates, All, SelfUpdating, Hidden, History, Themes, Settings };

enum class Hit {
    None, Nav, Search, SearchClear, Refresh, UpdateAll, Row, RowAction, RowMore,
    BannerStop, BannerRestart, BannerWinget, OpenHangar, HistClear,
    Seg, SetCheckNow, SetOpenData, SetOpenLog, SetRepo, SetManage, SetWingetRepair, ToastClose,
    ThemePick, AccentPick, ThemeExport, ThemeFolder, ThemeReload, ThemeBrowse,
    ScrollThumb, ScrollTrack,
    SelfRestart, SelfCheck, SelfRollback, WhatsNewClose,
};

// Segmented controls in Settings (HotRect.index)
enum Setting { SSchedule, SNotify, SNightly, STray, SStartup, SSelfUpdating, SUnknown, SReopen, SSelfChannel, SSelfAuto };

struct HotRect {
    D2D1_RECT_F r;
    Hit kind;
    int index;
    int arg;
    bool enabled;
};

static HWND g_hwnd;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_br;
static Theme g_t;
static bool g_rtOpaque = false;
static std::vector<ThemeDef> g_customThemes;

static void DiscardDevice();
static void ApplyTheme() {
    g_t = MakeTheme(FindTheme(g_cfg.theme, g_customThemes), g_cfg.accent);
    if (g_rt && g_rtOpaque != !g_t.backdrop) DiscardDevice();
    if (g_hwnd) { ApplyWindowTheme(g_hwnd, g_t); InvalidateRect(g_hwnd, nullptr, FALSE); }
}
static float g_scale = 1.0f;
static float g_W = 0, g_H = 0;

static std::vector<PkgState> g_pkgs;
static Inventory g_inv;                 // the latest scan (without per-row state)
static bool g_haveScan = false;         // a real scan arrived this session (not just the cache)
static Suite g_suite;
static int g_suiteCount = 0;

static Page g_page = Page::Updates;
static Page g_lastList = Page::Updates;  // where Esc goes back to from Settings and Themes
static std::wstring g_search;
static bool g_searchFocus = false;
static bool g_searchAll = false;
static bool g_suppressBack = false;
static bool g_caretOn = true;
static int g_focus = -1;                 // keyboard focus: index into g_visible
static bool g_keyboardMode = false;
static float g_scroll = 0, g_scrollTarget = 0, g_contentH = 0, g_viewH = 0;
static std::map<Page, float> g_savedScroll;
static float g_pageAnim = 1.0f;
static std::vector<int> g_visible;       // indices into g_pkgs on a list page
static int g_sectionAt = -1;             // on Updates: where "Not included" starts in g_visible
static float g_listTop = 0;              // content y of the first row (below the banners)

static std::vector<HistoryEntry> g_hist; // newest first
static std::vector<int> g_histVisible;

static std::vector<HotRect> g_hot;
static HotRect g_hover{{0, 0, 0, 0}, Hit::None, -1, 0, false};
static HotRect g_pressed{{0, 0, 0, 0}, Hit::None, -1, 0, false};
static bool g_tracking = false;

static std::wstring g_toast;
static bool g_toastBad = false;
static DWORD g_toastAt = 0;
static float g_toastAnim = 0;

// the queue as a whole
static int g_queueTotal = 0, g_queueDone = 0;
static std::vector<std::wstring> g_restartNeeded;   // apps that need a restart to finish
static std::wstring g_wingetStatus;                 // installing winget: progress or the error
static bool g_wingetSetupTried = false, g_wingetBusy = false;
static bool g_overnightRun = false;
static int g_overnightCount = 0;

enum : UINT_PTR { TIMER_ANIM = 1, TIMER_CARET, TIMER_PROCS, TIMER_SCHEDULE, TIMER_TOAST, TIMER_SELF, TIMER_HEALTHY, TIMER_MEMNOTE };
static bool g_animTimer = false;

// Restock's own updater
static SelfUpdateState g_self;
static DWORD g_selfCheckedTick = 0;
static bool g_applyWhenIdle = false;
static std::wstring g_whatsNew, g_whatsNewVersion;
static selfupdate::AfterUpdate g_after;
static std::vector<selfupdate::Previous> g_prevVersions;

static void Invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }
static bool WindowShown() { return g_hwnd && IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd); }
static void StartAnim() {
    if (!WindowShown()) return;
    if (!g_animTimer && g_hwnd) { SetTimer(g_hwnd, TIMER_ANIM, 15, nullptr); g_animTimer = true; }
    Invalidate();
}

static bool IsElevated() {
    static int e = -1;
    if (e < 0) {
        e = 0;
        HANDLE tok;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            TOKEN_ELEVATION te{};
            DWORD n = 0;
            if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n)) e = te.TokenIsElevated ? 1 : 0;
            CloseHandle(tok);
        }
    }
    return e == 1;
}

// ---------------------------------------------------------------- package rules
static PkgState* FindPkg(const std::wstring& key) {
    for (auto& s : g_pkgs) if (IEquals(s.p.key, key)) return &s;
    return nullptr;
}

static bool HasUpdate(const PkgState& s) { return !s.p.available.empty() && !s.p.id.empty() && !s.p.truncatedId; }

enum class Why { Ok, Suite, Ignored, Pinned, Skipped, SelfUpdating };

static Why Exclusion(const PkgState& s) {
    if (s.suite) return Why::Suite;
    if (g_cfg.ignored.count(s.p.key)) return Why::Ignored;
    if (g_cfg.pinned.count(s.p.key)) return Why::Pinned;
    auto sk = g_cfg.skipped.find(s.p.key);
    if (sk != g_cfg.skipped.end() && !s.p.available.empty() && CompareVersions(sk->second, s.p.available) == 0) return Why::Skipped;
    if (s.selfUpdating && !g_cfg.includeSelfUpdating && !g_cfg.forceInclude.count(s.p.key)) return Why::SelfUpdating;
    return Why::Ok;
}

// in "Update all", the schedule and the tray's count
static bool Eligible(const PkgState& s) { return HasUpdate(s) && Exclusion(s) == Why::Ok && !s.queued; }

static int UpdateCount() {
    int n = 0;
    for (auto& s : g_pkgs) if (HasUpdate(s) && Exclusion(s) == Why::Ok) ++n;
    return n;
}

static std::wstring JoinNames(const std::vector<std::wstring>& names, size_t max = 3) {
    std::wstring s;
    size_t shown = std::min(names.size(), names.size() > max ? max - 1 : max);
    for (size_t i = 0; i < shown; ++i) {
        if (i) s += (i + 1 == names.size()) ? L" and " : L", ";
        s += names[i];
    }
    if (names.size() > shown) s += L" and " + std::to_wstring(names.size() - shown) + L" more";
    return s;
}

// ---------------------------------------------------------------- toasts
static void Toast(const std::wstring& text, bool bad = false) {
    g_toast = text;
    g_toastBad = bad;
    g_toastAt = GetTickCount();
    if (g_hwnd) SetTimer(g_hwnd, TIMER_TOAST, bad ? 7000 : 4500, nullptr);
    StartAnim();
}

static void OpenUrl(const std::wstring& url) { ShellExecuteW(g_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// A Windows task dialog with your own button labels. Returns 1 for `yes`, 2 for `alt`, 0 for no.
static int AskDialog(const std::wstring& heading, const std::wstring& text, const std::wstring& yes, const std::wstring& no,
                     const std::wstring& alt = L"", bool warn = false) {
    TASKDIALOG_BUTTON buttons[3];
    int n = 0;
    buttons[n++] = {101, yes.c_str()};
    if (!alt.empty()) buttons[n++] = {102, alt.c_str()};
    buttons[n++] = {IDCANCEL, no.c_str()};
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = WindowShown() ? g_hwnd : nullptr;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"Restock";
    tc.pszMainInstruction = heading.c_str();
    tc.pszContent = text.c_str();
    tc.pButtons = buttons;
    tc.cButtons = (UINT)n;
    tc.nDefaultButton = 101;
    if (warn) tc.pszMainIcon = TD_WARNING_ICON;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) {
        return MessageBoxW(tc.hwndParent, (heading + L"\n\n" + text).c_str(), L"Restock", MB_OKCANCEL | MB_ICONQUESTION) == IDOK ? 1 : 0;
    }
    return pressed == 101 ? 1 : pressed == 102 ? 2 : 0;
}

// ---------------------------------------------------------------- Hangar (the suite's hub)
static std::wstring FindHangar() {
    // running?
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        std::wstring hit;
        for (BOOL ok = Process32FirstW(snap, &pe); ok && hit.empty(); ok = Process32NextW(snap, &pe))
            if (IEquals(pe.szExeFile, L"Hangar.exe")) hit = ProcessPath(pe.th32ProcessID);
        CloseHandle(snap);
        if (!hit.empty()) return hit;
    }
    std::vector<std::wstring> places = {
        KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\Hangar\\Hangar.exe",
        KnownFolder(FOLDERID_Programs) + L"\\Hangar.lnk",
        KnownFolder(FOLDERID_Desktop) + L"\\Hangar.lnk",
        KnownFolder(FOLDERID_Downloads) + L"\\Hangar.exe",
        ExeDir() + L"\\Hangar.exe",
    };
    for (auto& p : places) if (FileExists(p)) return p;
    return L"";
}

static void OpenHangar() {
    std::wstring h = FindHangar();
    if (!h.empty() && (INT_PTR)ShellExecuteW(g_hwnd, L"open", h.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32) return;
    Toast(L"Hangar isn't on this PC yet. Opening its download page.");
    OpenUrl(L"https://github.com/larosa023/Hangar/releases/latest");
}

// ---------------------------------------------------------------- updating
static UpdateRequest MakeRequest(const PkgState& s, bool closeApproved, bool overnight) {
    UpdateRequest r;
    r.key = s.p.key;
    r.id = s.p.id;
    r.source = s.p.source;
    r.name = s.p.name;
    r.from = s.p.version;
    r.to = s.p.available;
    r.runDir = s.p.runDir;
    r.closeApproved = closeApproved;
    r.reopen = g_cfg.reopen;
    r.overnight = overnight;
    r.includeUnknown = g_cfg.includeUnknown;
    return r;
}

static void QueueOne(PkgState& s, bool closeApproved, bool overnight = false) {
    if (s.queued || !HasUpdate(s)) return;
    s.queued = true;
    s.qstate = QueueState::Waiting;
    s.percent = -1;
    s.shownPercent = 0;
    s.opText = L"Waiting…";
    s.result = PkgState::None;
    s.resultText.clear();
    if (jobs::QueueSize() == 0) { g_queueTotal = 0; g_queueDone = 0; }
    ++g_queueTotal;
    jobs::Enqueue(MakeRequest(s, closeApproved, overnight));
}

// Asks once about every app in the batch that's open, then queues the batch
static void UpdateApps(const std::vector<int>& idx) {
    if (idx.empty()) return;
    std::vector<std::wstring> open;
    for (int i : idx) if (!g_pkgs[i].pids.empty()) open.push_back(g_pkgs[i].p.name);
    bool closeOk = true, skipOpen = false;
    if (!open.empty()) {
        std::wstring heading = JoinNames(open, 4) + (open.size() == 1 ? L" is open" : L" are open");
        std::wstring text = L"Restock will close " + std::wstring(open.size() == 1 ? L"it" : L"them") + L", update " +
                            (open.size() == 1 ? L"it" : L"them") + (g_cfg.reopen ? L" and open " + std::wstring(open.size() == 1 ? L"it" : L"them") + L" again." : L".") +
                            L" Save your work first.";
        int a = AskDialog(heading, text, L"Close and update", L"Cancel", idx.size() > open.size() ? L"Skip open apps" : L"");
        if (a == 0) return;
        skipOpen = a == 2;
        closeOk = a == 1;
    }
    int n = 0;
    for (int i : idx) {
        PkgState& s = g_pkgs[i];
        if (skipOpen && !s.pids.empty()) continue;
        // only what you saw listed as open is closed without asking again
        QueueOne(s, closeOk && !s.pids.empty());
        ++n;
    }
    if (n > 1) Toast(L"Updating " + std::to_wstring(n) + L" apps. You can keep working.");
    StartAnim();
}

static void UpdateAll() {
    std::vector<int> idx;
    for (size_t i = 0; i < g_pkgs.size(); ++i) if (Eligible(g_pkgs[i])) idx.push_back((int)i);
    if (idx.empty()) {
        Toast(jobs::QueueSize() ? L"Everything is already in the queue." : L"Everything is up to date.");
        return;
    }
    UpdateApps(idx);
}

static void RefreshTray();

static void RescanSoon(bool background = false) { jobs::Scan(g_cfg.includeUnknown, background); Invalidate(); }

// ---------------------------------------------------------------- row actions
enum class Action { None, Update, UpdateAnyway, Cancel, Website, Pinned };

static Action PrimaryAction(const PkgState& s, std::wstring* label, bool* enabled, int* styleOut) {
    *enabled = true;
    if (styleOut) *styleOut = 1;
    if (s.queued) {
        *label = s.qstate == QueueState::Waiting ? L"Remove" : L"Stop";
        return Action::Cancel;
    }
    if (HasUpdate(s)) {
        Why w = Exclusion(s);
        if (w == Why::Pinned) { *label = L"Pinned"; *enabled = false; return Action::Pinned; }
        if (w == Why::Ok) { *label = L"Update"; if (styleOut) *styleOut = 0; return Action::Update; }
        *label = L"Update";
        return Action::UpdateAnyway;
    }
    if (s.p.id.empty() && !s.p.url.empty() && IStartsWith(s.p.url, L"http")) { *label = L"Website"; if (styleOut) *styleOut = 2; return Action::Website; }
    return Action::None;
}

static void DoPrimary(int i) {
    if (i < 0 || i >= (int)g_pkgs.size()) return;
    PkgState& s = g_pkgs[i];
    std::wstring label;
    bool enabled;
    Action a = PrimaryAction(s, &label, &enabled, (int*)nullptr);
    if (!enabled) return;
    switch (a) {
        case Action::Update:
        case Action::UpdateAnyway:
            UpdateApps({i});
            break;
        case Action::Cancel:
            if (s.qstate != QueueState::Waiting &&
                AskDialog(L"Stop updating " + s.p.name + L"?", L"winget stops right away. If the installer has already started, it may finish by itself.",
                          L"Stop", L"Keep going", L"", true) != 1)
                break;
            jobs::Cancel(s.p.key);
            if (s.qstate != QueueState::Waiting) s.opText = L"Stopping…";
            break;
        case Action::Website: OpenUrl(s.p.url); break;
        default: break;
    }
    Invalidate();
}

static void CopyText(const std::wstring& t) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

enum MenuCmd : UINT { MUpdate = 1, MSkip, MUnskip, MPin, MUnpin, MIgnore, MUnignore, MInclude, MExclude, MFolder, MCopyId, MWebsite, MWinstall };

static void RowMenu(int i, POINT* at) {
    if (i < 0 || i >= (int)g_pkgs.size()) return;
    PkgState& s = g_pkgs[i];
    const std::wstring& k = s.p.key;
    HMENU m = CreatePopupMenu();
    bool pinned = g_cfg.pinned.count(k) > 0, ignored = g_cfg.ignored.count(k) > 0;
    auto sk = g_cfg.skipped.find(k);
    bool skipping = sk != g_cfg.skipped.end();
    if (HasUpdate(s) && !s.queued && !pinned)
        AppendMenuW(m, MF_STRING, MUpdate, (L"Update to " + s.p.available).c_str());
    if (HasUpdate(s) && !skipping) AppendMenuW(m, MF_STRING, MSkip, (L"Skip version " + s.p.available).c_str());
    if (skipping) AppendMenuW(m, MF_STRING, MUnskip, (L"Stop skipping " + sk->second).c_str());
    if (!s.p.id.empty()) AppendMenuW(m, MF_STRING, pinned ? MUnpin : MPin, pinned ? L"Unpin (allow updates)" : L"Pin (never update)");
    AppendMenuW(m, MF_STRING, ignored ? MUnignore : MIgnore, ignored ? L"Stop ignoring" : L"Ignore this app");
    if (s.selfUpdating && !g_cfg.includeSelfUpdating)
        AppendMenuW(m, MF_STRING, g_cfg.forceInclude.count(k) ? MExclude : MInclude,
                    g_cfg.forceInclude.count(k) ? L"Leave it to its own updater" : L"Include in Update all");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring folder = DirExists(s.p.location) ? s.p.location : s.p.runDir;
    AppendMenuW(m, MF_STRING | (DirExists(folder) ? 0 : MF_GRAYED), MFolder, L"Open install folder");
    if (!s.p.id.empty()) {
        AppendMenuW(m, MF_STRING, MCopyId, L"Copy winget ID");
        AppendMenuW(m, MF_STRING, MWinstall, L"View on winstall.app");
    }
    if (!s.p.url.empty() && IStartsWith(s.p.url, L"http")) AppendMenuW(m, MF_STRING, MWebsite, L"Publisher's website");
    POINT pt;
    if (at) pt = *at; else GetCursorPos(&pt);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case MUpdate: UpdateApps({i}); break;
        case MSkip: g_cfg.skipped[k] = s.p.available; Toast(L"Skipping " + s.p.name + L" " + s.p.available + L". A newer version will show up again."); break;
        case MUnskip: g_cfg.skipped.erase(k); break;
        case MPin: g_cfg.pinned.insert(k); Toast(s.p.name + L" is pinned. Restock won't update it."); break;
        case MUnpin: g_cfg.pinned.erase(k); break;
        case MIgnore: g_cfg.ignored.insert(k); Toast(s.p.name + L" is hidden. Find it under Pinned & ignored."); break;
        case MUnignore: g_cfg.ignored.erase(k); break;
        case MInclude: g_cfg.forceInclude.insert(k); break;
        case MExclude: g_cfg.forceInclude.erase(k); break;
        case MFolder: ShellExecuteW(g_hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case MCopyId: CopyText(s.p.id); Toast(L"Copied " + s.p.id); break;
        case MWinstall: OpenUrl(L"https://winstall.app/apps/" + s.p.id); break;
        case MWebsite: OpenUrl(s.p.url); break;
        default: return;
    }
    ConfigSave();
    RefreshTray();
    StartAnim();
}

// ---------------------------------------------------------------- applying scans
static UINT IconPx() { return (UINT)std::min(96.0f, std::ceil(40 * g_scale)); }

static void ApplyInventory(const Inventory& inv) {
    g_inv = inv;
    std::vector<PkgState> next;
    next.reserve(inv.pkgs.size());
    for (auto& p : inv.pkgs) {
        PkgState s;
        if (PkgState* old = FindPkg(p.key)) {
            s = std::move(*old);
            old->icon = nullptr;   // ownership moves
            old->queued = false;
            old->result = PkgState::None;
            bool iconChanged = !IEquals(s.p.iconPath, p.iconPath) || s.p.iconIndex != p.iconIndex;
            s.p = p;
            if (iconChanged) { SafeRelease(s.icon); s.iconPixels.clear(); s.iconRequested = false; }
        } else {
            s.p = p;
        }
        s.suite = !g_suite.Empty() && g_suite.Contains(p);
        s.selfUpdating = IsSelfUpdating(p);
        next.push_back(std::move(s));
    }
    // rows still in the queue (or that just finished) stay even if the scan dropped them
    for (auto& old : g_pkgs) {
        bool kept = false;
        for (auto& n : next) kept |= IEquals(n.p.key, old.p.key);
        if (!kept && !old.p.key.empty() && (old.queued || old.result != PkgState::None)) { next.push_back(std::move(old)); old.icon = nullptr; }
    }
    for (auto& s : g_pkgs) SafeRelease(s.icon);
    g_pkgs = std::move(next);
    g_suiteCount = 0;
    for (auto& s : g_pkgs) g_suiteCount += s.suite ? 1 : 0;
}

static void ApplySuite(const std::wstring& text) {
    Suite s;
    if (!s.Load(text)) return;
    g_suite = s;
    g_suiteCount = 0;
    for (auto& p : g_pkgs) { p.suite = g_suite.Contains(p.p); g_suiteCount += p.suite ? 1 : 0; }
}

static std::wstring TrayTip() {
    int n = UpdateCount();
    if (jobs::QueueSize()) return L"Restock · updating " + std::to_wstring(g_queueDone + 1) + L" of " + std::to_wstring(std::max(g_queueTotal, 1));
    if (jobs::Scanning()) return L"Restock · checking for updates";
    if (n == 0) return L"Restock · everything is up to date";
    return L"Restock · " + std::to_wstring(n) + (n == 1 ? L" update" : L" updates");
}

static void RefreshTray() {
    static std::wstring last;
    std::wstring tip = TrayTip();
    if (tip == last) return;
    last = tip;
    tray::SetTip(tip);
}

// "N updates are waiting", once per set of updates
static void MaybeNotify() {
    if (!g_cfg.notify) return;
    if (WindowShown() && GetForegroundWindow() == g_hwnd) return;
    std::vector<std::wstring> names, sig;
    for (auto& s : g_pkgs)
        if (HasUpdate(s) && Exclusion(s) == Why::Ok && !s.queued) { names.push_back(s.p.name); sig.push_back(s.p.key + L"@" + s.p.available); }
    if (names.empty()) { g_cfg.notified.clear(); return; }
    std::sort(sig.begin(), sig.end());
    std::wstring key;
    for (auto& x : sig) key += x + L";";
    if (key == g_cfg.notified) return;
    g_cfg.notified = key;
    ConfigSave();
    std::wstring title = std::to_wstring(names.size()) + (names.size() == 1 ? L" update is waiting" : L" updates are waiting");
    tray::Notify(title, JoinNames(names) + L". Click to review.");
}

// The overnight run: everything eligible that won't need you (not open, not installed for all users)
static void QueueOvernight() {
    int n = 0;
    for (auto& s : g_pkgs) {
        if (!Eligible(s)) continue;
        if (s.p.machine && !IsElevated()) continue;   // its installer would wait for a UAC prompt nobody answers
        QueueOne(s, false, true);
        ++n;
    }
    g_overnightRun = n > 0;
    g_overnightCount = 0;
    Log(L"overnight: %d update(s) queued", n);
}

// ---------------------------------------------------------------- render target
static bool g_iconsLost = false;
static ID2D1Layer* g_previewLayer = nullptr;

static void DiscardDevice() {
    for (auto& s : g_pkgs) if (s.icon) { SafeRelease(s.icon); g_iconsLost = true; }
    SafeRelease(g_previewLayer);
    SafeRelease(g_br);
    SafeRelease(g_rt);
}

static bool EnsureDevice() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_rtOpaque = !g_t.backdrop;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, g_rtOpaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED));
    UINT dpi = GetDpiForWindow(g_hwnd);
    props.dpiX = props.dpiY = (float)dpi;
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)),
                                           D2D1_PRESENT_OPTIONS_NONE), &g_rt))) return false;
    g_rt->SetTextAntialiasMode(g_rtOpaque ? D2D1_TEXT_ANTIALIAS_MODE_DEFAULT : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_br);
    if (g_iconsLost) {
        // icon pixels are freed once uploaded: extract them again for the new device as rows appear
        g_iconsLost = false;
        for (auto& s : g_pkgs) if (!s.icon && s.iconPixels.empty()) s.iconRequested = false;
    }
    return true;
}

static ID2D1Bitmap* IconBitmap(PkgState& s) {
    if (!s.iconRequested) {
        s.iconRequested = true;
        jobs::FetchIcon(s.p.key, s.p.iconPath, s.p.iconIndex, IconPx());
    }
    if (s.icon || s.iconPixels.empty() || !g_rt) return s.icon;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    g_rt->CreateBitmap(D2D1::SizeU(s.iconW, s.iconH), s.iconPixels.data(), s.iconW * 4, bp, &s.icon);
    if (s.icon) std::vector<uint8_t>().swap(s.iconPixels);
    return s.icon;
}

// ---------------------------------------------------------------- drawing helpers
static Canvas g_c;

static void AddHot(const D2D1_RECT_F& r, Hit kind, int index = -1, int arg = 0, bool enabled = true) {
    g_hot.push_back({r, kind, index, arg, enabled});
}
static bool IsHover(Hit kind, int index = -1, int arg = 0) {
    return g_hover.kind == kind && g_hover.index == index && g_hover.arg == arg;
}
static bool IsPressed(Hit kind, int index = -1, int arg = 0) {
    return g_pressed.kind == kind && g_pressed.index == index && g_pressed.arg == arg && IsHover(kind, index, arg);
}

enum class Btn { Primary, Secondary, Subtle, Danger };

static float ButtonWidth(const std::wstring& label, wchar_t glyph) {
    float w = MeasureWidth(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28;
    if (glyph) w += label.empty() ? 6 : 22;
    return std::max(w, label.empty() ? 34.0f : 76.0f);
}

static void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph, Btn style, Hit kind, int index = -1, int arg = 0, bool enabled = true) {
    bool hot = enabled && IsHover(kind, index, arg);
    bool down = enabled && IsPressed(kind, index, arg);
    D2D1_COLOR_F bg, fg;
    switch (style) {
        case Btn::Primary: bg = hot ? g_t.accentHover : g_t.accent; fg = g_t.onAccent; break;
        case Btn::Danger: bg = hot ? Alpha(g_t.bad, 0.22f) : Alpha(g_t.bad, 0.12f); fg = g_t.bad; break;
        case Btn::Subtle: bg = hot ? g_t.surfaceHover : Alpha(g_t.surface, 0.0f); fg = g_t.text; break;
        default: bg = hot ? g_t.surfaceStrong : Mix(g_t.surface, g_t.surfaceStrong, 0.55f); fg = g_t.text; break;
    }
    if (!enabled) { bg = Alpha(g_t.surfaceStrong, 0.5f); fg = g_t.faint; }
    if (down) bg = Mix(bg, g_t.dark ? Rgb(0x000000) : Rgb(0xFFFFFF), 0.12f);
    g_c.FillRound(r, 6, bg);
    if (style == Btn::Secondary && enabled) g_c.StrokeRound(r, 6, g_t.divider);
    float cx = r.left, w = r.right - r.left;
    auto* tf = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (glyph && label.empty()) {
        g_c.Glyph(glyph, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, fg);
    } else if (glyph) {
        float tw = MeasureWidth(label, tf) + 22;
        float x = cx + (w - tw) / 2;
        g_c.Glyph(glyph, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true), D2D1::RectF(x, r.top, x + 18, r.bottom), fg);
        g_c.Text(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 22, r.top, r.right, r.bottom), fg);
    } else {
        g_c.Text(label, tf, r, fg);
    }
    AddHot(r, kind, index, arg, enabled);
}

static float Pill(float x, float y, const std::wstring& text, D2D1_COLOR_F color, bool rightAlign = false) {
    if (text.empty()) return 0;
    auto* f = Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    float w = MeasureWidth(text, f) + 16;
    if (rightAlign) x -= w;
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + 20);
    g_c.FillRound(r, 10, Alpha(color, g_t.dark ? 0.16f : 0.12f));
    g_c.Text(text, f, r, color);
    return w;
}

static void ProgressBar(const D2D1_RECT_F& r, float pct, D2D1_COLOR_F color) {
    g_c.FillRound(r, (r.bottom - r.top) / 2, g_t.surfaceStrong);
    float w = r.right - r.left;
    if (pct < 0) {
        float t = (GetTickCount() % 1400) / 1400.0f;
        float seg = w * 0.3f;
        float x = r.left + (w + seg) * t - seg;
        D2D1_RECT_F s = D2D1::RectF(std::max(r.left, x), r.top, std::min(r.right, x + seg), r.bottom);
        if (s.right > s.left) g_c.FillRound(s, (r.bottom - r.top) / 2, color);
    } else {
        D2D1_RECT_F s = r;
        s.right = r.left + w * std::min(1.0f, pct / 100.0f);
        if (s.right - s.left > 1) g_c.FillRound(s, (r.bottom - r.top) / 2, color);
    }
}

static void DrawPkgIcon(PkgState& s, const D2D1_RECT_F& r) {
    if (ID2D1Bitmap* bmp = IconBitmap(s)) {
        D2D1_SIZE_F sz = bmp->GetSize();
        float w = r.right - r.left, h = r.bottom - r.top;
        float k = std::min(w / sz.width, h / sz.height);
        float dw = sz.width * k, dh = sz.height * k;
        D2D1_RECT_F d = D2D1::RectF(r.left + (w - dw) / 2, r.top + (h - dh) / 2, r.left + (w + dw) / 2, r.top + (h + dh) / 2);
        g_rt->DrawBitmap(bmp, d, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    // placeholder: a soft tile with the app's initial, colored from its name
    uint32_t h = 0;
    for (wchar_t c : s.p.name) h = h * 31 + c;
    static const uint32_t palette[] = {0x5B8DEF, 0x9B6CF2, 0xE86A92, 0xF29B4B, 0x3FB98E, 0x47B4D6, 0xD9634C, 0x7A86A8};
    D2D1_COLOR_F col = Rgb(palette[h % 8]);
    float rad = (r.right - r.left) * 0.24f;
    g_c.FillRound(r, rad, Alpha(col, 0.22f));
    std::wstring init = s.p.name.substr(0, 1);
    g_c.Text(init, Format((r.bottom - r.top) * 0.46f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), r, col);
}

static std::wstring CleanNotes(const std::wstring& md) {
    std::wstring out;
    size_t i = 0;
    int blank = 0;
    while (i <= md.size()) {
        size_t e = md.find(L'\n', i);
        if (e == std::wstring::npos) e = md.size();
        std::wstring line = md.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = Trim(line);
        if (t.empty()) { if (++blank <= 1 && !out.empty()) out += L"\n"; continue; }
        blank = 0;
        size_t indent = line.find_first_not_of(L" \t");
        while (!t.empty() && t[0] == L'#') t.erase(0, 1);
        t = Trim(t);
        if (t.size() > 1 && (t[0] == L'-' || t[0] == L'*' || t[0] == L'+') && t[1] == L' ') t = std::wstring(indent >= 2 ? L"    \x25E6 " : L"\x2022  ") + Trim(t.substr(2));
        if (t == L"---" || t == L"***") continue;
        std::wstring s;
        for (size_t k = 0; k < t.size(); ++k) {
            if (t[k] == L'!' && k + 1 < t.size() && t[k + 1] == L'[') continue;
            if (t[k] == L'[') {
                size_t close = t.find(L"](", k);
                size_t paren = close == std::wstring::npos ? std::wstring::npos : t.find(L')', close);
                if (close != std::wstring::npos && paren != std::wstring::npos) { s += t.substr(k + 1, close - k - 1); k = paren; continue; }
            }
            if (t[k] == L'*' || t[k] == L'`' || (t[k] == L'_' && k + 1 < t.size() && t[k + 1] == L'_')) { if (t[k] == L'_') ++k; continue; }
            s += t[k];
        }
        out += s + L"\n";
    }
    while (!out.empty() && out.back() == L'\n') out.pop_back();
    if (out.size() > 4000) out = out.substr(0, 4000) + L"…";
    return out;
}

static std::wstring Ago(long long t) {
    if (!t) return L"never";
    long long ago = (long long)time(nullptr) - t;
    if (ago < 90) return L"just now";
    if (ago < 3600) return std::to_wstring(ago / 60) + L" min ago";
    if (ago < 86400) return std::to_wstring(ago / 3600) + L" h ago";
    long long d = ago / 86400;
    return std::to_wstring(d) + (d == 1 ? L" day ago" : L" days ago");
}

static SYSTEMTIME LocalTime(long long unix) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)unix * 10000000ULL + 116444736000000000ULL;
    FILETIME ft{u.LowPart, u.HighPart}, lt;
    FileTimeToLocalFileTime(&ft, &lt);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&lt, &st);
    return st;
}

static std::wstring DayLabel(long long unix) {
    SYSTEMTIME st = LocalTime(unix), now;
    GetLocalTime(&now);
    SYSTEMTIME y = LocalTime((long long)time(nullptr) - 86400);
    auto same = [](const SYSTEMTIME& a, const SYSTEMTIME& b) { return a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay; };
    if (same(st, now)) return L"Today";
    if (same(st, y)) return L"Yesterday";
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t b[48];
    swprintf(b, 48, L"%ls %d, %d", mon[(st.wMonth + 11) % 12], st.wDay, st.wYear);
    return b;
}

static std::wstring ClockTime(long long unix) {
    SYSTEMTIME st = LocalTime(unix);
    wchar_t b[32];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, b, 32);
    return b;
}

// ---------------------------------------------------------------- sidebar
static const float kSidebar = 244;
static const float kHeader = 76;
static std::vector<std::wstring> g_navIds;   // arg -> nav id

static Page NavPage(const std::wstring& id) {
    if (id == L"all") return Page::All;
    if (id == L"self") return Page::SelfUpdating;
    if (id == L"hidden") return Page::Hidden;
    if (id == L"history") return Page::History;
    if (id == L"themes") return Page::Themes;
    if (id == L"settings") return Page::Settings;
    return Page::Updates;
}

static std::wstring PageId(Page p) {
    switch (p) {
        case Page::All: return L"all";
        case Page::SelfUpdating: return L"self";
        case Page::Hidden: return L"hidden";
        case Page::History: return L"history";
        case Page::Themes: return L"themes";
        case Page::Settings: return L"settings";
        default: return L"updates";
    }
}

static void NavItem(float y, const std::wstring& id, const std::wstring& label, wchar_t glyph, int count, bool badge, int arg) {
    D2D1_RECT_F r = D2D1::RectF(10, y, kSidebar - 10, y + 36);
    bool sel = id != L"hangar" && NavPage(id) == g_page;
    bool hot = IsHover(Hit::Nav, -1, arg);
    if (sel) g_c.FillRound(r, 6, g_t.dark ? Rgb(0xFFFFFF, 0.08f) : Rgb(0x000000, 0.06f));
    else if (hot) g_c.FillRound(r, 6, g_t.dark ? Rgb(0xFFFFFF, 0.04f) : Rgb(0x000000, 0.035f));
    if (sel) g_c.FillRound(D2D1::RectF(r.left, y + 10, r.left + 3, y + 26), 1.5f, g_t.accent);
    g_c.Glyph(glyph, Format(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 10, y, r.left + 34, y + 36),
              sel ? g_t.accent : g_t.subtext);
    g_c.Text(label, Format(13.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 42, y, r.right - 40, y + 36), g_t.text);
    if (count >= 0) {
        std::wstring n = std::to_wstring(count);
        if (badge && count > 0) {
            auto* f = Format(11.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            float w = std::max(22.0f, MeasureWidth(n, f) + 12);
            D2D1_RECT_F b = D2D1::RectF(r.right - 10 - w, y + 8, r.right - 10, y + 28);
            g_c.FillRound(b, 10, g_t.accent);
            g_c.Text(n, f, b, g_t.onAccent);
        } else if (!badge) {
            g_c.Text(n, Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.left, y, r.right - 12, y + 36), g_t.faint);
        }
    }
    AddHot(r, Hit::Nav, -1, arg);
}

static std::wstring StatusLine() {
    if (g_wingetBusy) return g_wingetStatus;
    if (jobs::Scanning()) return L"Checking for updates…";
    if (int n = jobs::QueueSize()) return L"Updating · " + std::to_wstring(n) + L" to go";
    if (!g_inv.scannedAt) return L"";
    return L"Checked " + Ago(g_inv.scannedAt);
}

static int CountPage(Page p);

static void DrawSidebar() {
    g_c.Fill(D2D1::RectF(0, 0, kSidebar, g_H), g_t.sidebar);
    g_c.Line(kSidebar - 0.5f, 0, kSidebar - 0.5f, g_H, g_t.divider);
    // brand: a box with an arrow, in the accent color
    D2D1_RECT_F logo = D2D1::RectF(22, 22, 50, 50);
    g_c.FillRound(logo, 8, g_t.accent);
    ID2D1PathGeometry* geo = nullptr;
    if (SUCCEEDED(d2d->CreatePathGeometry(&geo))) {
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(geo->Open(&s))) {
            // a crate
            s->BeginFigure(D2D1::Point2F(29, 34), D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(D2D1::Point2F(43, 34));
            s->AddLine(D2D1::Point2F(43, 44));
            s->AddLine(D2D1::Point2F(29, 44));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            // an arrow up out of it
            s->BeginFigure(D2D1::Point2F(36, 26), D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(D2D1::Point2F(40.5f, 30.5f));
            s->AddLine(D2D1::Point2F(37.3f, 30.5f));
            s->AddLine(D2D1::Point2F(37.3f, 35));
            s->AddLine(D2D1::Point2F(34.7f, 35));
            s->AddLine(D2D1::Point2F(34.7f, 30.5f));
            s->AddLine(D2D1::Point2F(31.5f, 30.5f));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            s->Release();
            g_br->SetColor(g_t.onAccent);
            g_rt->FillGeometry(geo, g_br);
            g_br->SetColor(g_t.accent);
            g_rt->FillRectangle(D2D1::RectF(34.7f, 34, 37.3f, 36.5f), g_br);
        }
        geo->Release();
    }
    g_c.Text(L"Restock", Format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(60, 20, kSidebar, 40), g_t.text);
    g_c.Text(L"Updates for everything else", Format(11.5f), D2D1::RectF(60, 38, kSidebar, 54), g_t.subtext);

    g_navIds.clear();
    float y = 76;
    auto item = [&](const std::wstring& id, const std::wstring& label, wchar_t glyph, int count, bool badge) {
        g_navIds.push_back(id);
        NavItem(y, id, label, glyph, count, badge, (int)g_navIds.size() - 1);
        y += 38;
    };
    item(L"updates", L"Updates", icon::Update, UpdateCount(), true);
    item(L"all", L"All apps", icon::Library, CountPage(Page::All), false);
    item(L"self", L"Self-updating", icon::Sync, CountPage(Page::SelfUpdating), false);
    item(L"hidden", L"Pinned & ignored", icon::Pin, CountPage(Page::Hidden), false);
    item(L"history", L"History", icon::History, -1, false);

    // bottom
    float by = g_H - 82;
    float ty = by - 38;          // Themes above Settings
    float hy = ty - 38;          // Open Hangar above Themes
    if (g_self.kind == SelfUpdateState::Ready && y + 150 < hy) {
        D2D1_RECT_F b = D2D1::RectF(12, hy - 92, kSidebar - 12, hy - 8);
        g_c.FillRound(b, 8, Alpha(g_t.accent, 0.14f));
        g_c.StrokeRound(b, 8, Alpha(g_t.accent, 0.35f));
        g_c.Text(L"Restock " + g_self.version + L" is ready", Format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(b.left + 12, b.top + 8, b.right - 8, b.top + 28), g_t.text);
        g_c.Text(jobs::QueueSize() ? L"Installs after the current updates" : L"Takes a second · you stay where you are", Format(11.5f), D2D1::RectF(b.left + 12, b.top + 27, b.right - 8, b.top + 44), g_t.subtext);
        Button(D2D1::RectF(b.left + 10, b.bottom - 34, b.right - 10, b.bottom - 8), L"Restart to update", icon::Update, Btn::Primary, Hit::SelfRestart);
    }
    if (g_suiteCount > 0 && y + 10 < hy) {
        g_navIds.push_back(L"hangar");
        NavItem(hy, L"hangar", L"Suite apps in Hangar", icon::Link, g_suiteCount, false, (int)g_navIds.size() - 1);
    }
    if (y + 10 < ty) {
        g_navIds.push_back(L"themes");
        NavItem(ty, L"themes", L"Themes", icon::Color, -1, false, (int)g_navIds.size() - 1);
    }
    if (y + 10 < by) {
        g_navIds.push_back(L"settings");
        NavItem(by, L"settings", L"Settings", icon::Settings, -1, false, (int)g_navIds.size() - 1);
    }
    g_c.Text(StatusLine(), Format(11.5f), D2D1::RectF(24, g_H - 40, kSidebar - 12, g_H - 18), g_t.faint);
}

// ---------------------------------------------------------------- list pages
static bool OnPage(const PkgState& s, Page p) {
    if (s.suite) return false;
    bool ignored = g_cfg.ignored.count(s.p.key) > 0;
    switch (p) {
        case Page::Updates: return !ignored && (HasUpdate(s) || s.queued || s.result != PkgState::None);
        case Page::All: return !ignored;
        case Page::SelfUpdating: return !ignored && s.selfUpdating;
        case Page::Hidden: return ignored || g_cfg.pinned.count(s.p.key) || g_cfg.skipped.count(s.p.key);
        default: return false;
    }
}

static int CountPage(Page p) {
    int n = 0;
    for (auto& s : g_pkgs) n += OnPage(s, p) ? 1 : 0;
    return n;
}

static bool MatchesSearch(const PkgState& s) {
    return g_search.empty() || IContains(s.p.name, g_search) || IContains(s.p.id, g_search) || IContains(s.p.publisher, g_search);
}

static void ComputeVisible() {
    g_visible.clear();
    g_sectionAt = -1;
    std::vector<int> main, rest;
    for (size_t i = 0; i < g_pkgs.size(); ++i) {
        const PkgState& s = g_pkgs[i];
        if (!OnPage(s, g_page) || !MatchesSearch(s)) continue;
        // on Updates, what "Update all" leaves out goes in its own section below
        bool included = g_page != Page::Updates || s.queued || s.result != PkgState::None || Exclusion(s) == Why::Ok;
        (included ? main : rest).push_back((int)i);
    }
    g_visible = main;
    if (!rest.empty()) {
        g_sectionAt = (int)g_visible.size();
        g_visible.insert(g_visible.end(), rest.begin(), rest.end());
    }
}

static void ComputeHistoryVisible() {
    g_histVisible.clear();
    for (size_t i = 0; i < g_hist.size(); ++i)
        if (g_search.empty() || IContains(g_hist[i].name, g_search)) g_histVisible.push_back((int)i);
}

static std::wstring PageTitle() {
    switch (g_page) {
        case Page::All: return L"All apps";
        case Page::SelfUpdating: return L"Self-updating";
        case Page::Hidden: return L"Pinned & ignored";
        case Page::History: return L"History";
        default: return L"Updates";
    }
}

static std::wstring PageSubtitle() {
    size_t n = g_page == Page::History ? g_histVisible.size() : g_visible.size();
    switch (g_page) {
        case Page::Updates: {
            int u = UpdateCount();
            if (!g_haveScan && g_inv.pkgs.empty()) return L"Looking for updates…";
            return u == 0 ? L"Everything is up to date" : std::to_wstring(u) + (u == 1 ? L" update ready" : L" updates ready");
        }
        case Page::SelfUpdating: return L"These apps update themselves, so Restock leaves them alone unless you include them";
        case Page::Hidden: return L"Apps you pinned, ignored, or are skipping a version of";
        case Page::History: return std::to_wstring(n) + (n == 1 ? L" update" : L" updates");
        default: return std::to_wstring(n) + (n == 1 ? L" app" : L" apps") + (g_suiteCount ? L" · your " + std::to_wstring(g_suiteCount) + L" suite apps are in Hangar" : L"");
    }
}

struct StatusInfo { std::wstring label; D2D1_COLOR_F color; };

static StatusInfo Status(const PkgState& s) {
    if (s.queued) return {s.qstate == QueueState::Waiting ? L"Waiting" : L"", g_t.faint};
    switch (s.result) {
        case PkgState::Ok: return {L"Updated", g_t.good};
        case PkgState::Restart: return {L"Restart needed", g_t.warn};
        case PkgState::Failed: return {L"Failed", g_t.bad};
        case PkgState::Skipped: return {L"Skipped", g_t.faint};
        default: break;
    }
    if (g_cfg.ignored.count(s.p.key)) return {L"Ignored", g_t.faint};
    if (g_cfg.pinned.count(s.p.key)) return {L"Pinned", g_t.info};
    Why w = Exclusion(s);
    if (w == Why::Skipped) return {L"Skipping " + s.p.available, g_t.faint};
    if (HasUpdate(s) && w == Why::SelfUpdating) return {L"Updates itself", g_t.info};
    if (HasUpdate(s)) return {L"Update available", g_t.warn};
    if (s.selfUpdating) return {L"Updates itself", g_t.info};
    if (s.p.id.empty()) return {L"Not in winget", g_t.faint};
    if (s.p.truncatedId) return {L"ID unreadable", g_t.faint};
    return {L"Up to date", g_t.faint};
}

static std::wstring SourceLabel(const PkgState& s) {
    if (s.p.source.empty()) return L"";
    if (IEquals(s.p.source, L"msstore")) return L"Store";
    return s.p.source;
}

static std::wstring VersionLine(const PkgState& s) {
    std::wstring v = s.p.version.empty() ? L"?" : s.p.version;
    if (IEquals(v, L"Unknown")) v = L"?";
    if (HasUpdate(s)) return v + L"  \x2192  " + s.p.available;
    return v;
}

static void DrawRow(int i, const D2D1_RECT_F& r, bool focused) {
    PkgState& s = g_pkgs[i];
    D2D1_COLOR_F bg = Mix(g_t.surface, g_t.surfaceHover, s.hover);
    g_c.FillRound(r, 8, bg);
    if (focused && g_keyboardMode) g_c.StrokeRound(r, 8, g_t.accent, 2);
    AddHot(r, Hit::Row, i);
    float h = r.bottom - r.top;
    float x = r.left + 14, cy = r.top + h / 2;
    DrawPkgIcon(s, D2D1::RectF(x, cy - 16, x + 32, cy + 16));
    float w = r.right - r.left;
    bool narrow = w < 720;
    // right side: more button, primary button, status, source, version
    D2D1_RECT_F more = D2D1::RectF(r.right - 12 - 32, cy - 16, r.right - 12, cy + 16);
    Button(more, L"", icon::More, Btn::Subtle, Hit::RowMore, i);
    std::wstring label;
    bool en;
    int style = 1;
    Action act = PrimaryAction(s, &label, &en, &style);
    float right = more.left - 8;
    if (act != Action::None) {
        float bw = ButtonWidth(label, act == Action::Update || act == Action::UpdateAnyway ? icon::Update : 0);
        D2D1_RECT_F btn = D2D1::RectF(right - bw, cy - 15, right, cy + 15);
        Button(btn, label, act == Action::Update || act == Action::UpdateAnyway ? icon::Update : 0,
               style == 0 ? Btn::Primary : style == 2 ? Btn::Subtle : Btn::Secondary, Hit::RowAction, i, 0, en);
        right = btn.left - 14;
    } else {
        right -= 6;
    }
    float colStatus = right - 150;
    float colSource = colStatus - 80;
    float colVer = colSource - (narrow ? 0 : 190);
    float textRight = narrow ? colStatus - 10 : colVer - 14;
    g_c.Text(s.p.name, Format(14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 46, r.top + 10, textRight, r.top + 32), g_t.text);
    std::wstring sub = s.p.publisher;
    if (!s.p.id.empty()) sub += (sub.empty() ? L"" : L" · ") + s.p.id;
    if (narrow) sub = VersionLine(s) + (sub.empty() ? L"" : L" · " + sub);
    if (!s.resultText.empty() && s.result != PkgState::None && !s.queued) sub = s.resultText;
    D2D1_COLOR_F subColor = s.result == PkgState::Failed && !s.queued ? g_t.bad : g_t.subtext;
    g_c.Text(sub, Format(12.5f), D2D1::RectF(x + 46, r.top + 32, textRight, r.top + 52), subColor);
    if (!narrow) g_c.Text(VersionLine(s), Format(12.5f), D2D1::RectF(colVer, r.top, colSource - 8, r.bottom), HasUpdate(s) ? g_t.warn : g_t.subtext);
    if (!narrow) {
        std::wstring src = SourceLabel(s);
        if (!src.empty()) Pill(colSource, cy - 10, src, g_t.subtext);
    }
    if (s.queued && s.qstate != QueueState::Waiting) {
        float pct = s.percent < 0 ? -1 : s.shownPercent;
        std::wstring t = s.opText + (s.percent >= 0 ? L"  " + std::to_wstring(s.percent) + L"%" : L"");
        g_c.Text(t, Format(12), D2D1::RectF(colStatus, r.top + 12, right, r.top + 32), g_t.subtext);
        ProgressBar(D2D1::RectF(colStatus, r.top + 38, right, r.top + 42), pct, g_t.accent);
    } else {
        StatusInfo st = Status(s);
        if (!s.pids.empty() && (HasUpdate(s) || s.queued) && st.label != L"Waiting") st = {L"Open", g_t.good};
        Pill(colStatus, cy - 10, st.label, st.color);
    }
}

static void DrawHistoryRow(const HistoryEntry& e, const D2D1_RECT_F& r) {
    g_c.FillRound(r, 8, g_t.surface);
    float x = r.left + 14, cy = (r.top + r.bottom) / 2;
    if (PkgState* s = FindPkg(e.key)) DrawPkgIcon(*s, D2D1::RectF(x, cy - 14, x + 28, cy + 14));
    else {
        PkgState tmp;
        tmp.p.name = e.name;
        tmp.iconRequested = true;
        DrawPkgIcon(tmp, D2D1::RectF(x, cy - 14, x + 28, cy + 14));
    }
    float colStatus = r.right - 150, colTime = colStatus - 90, colVer = colTime - 200;
    bool narrow = r.right - r.left < 700;
    float textRight = narrow ? colTime - 10 : colVer - 12;
    g_c.Text(e.name, Format(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 42, r.top + 8, textRight, r.top + 28), g_t.text);
    std::wstring sub = e.message.empty() ? L"" : e.message;
    if (e.overnight) sub = L"Overnight" + std::wstring(sub.empty() ? L"" : L" · ") + sub;
    g_c.Text(sub, Format(12), D2D1::RectF(x + 42, r.top + 28, textRight, r.top + 46), e.result == L"failed" ? g_t.bad : g_t.subtext);
    if (!narrow) {
        std::wstring v = (e.from.empty() ? L"?" : e.from) + L"  \x2192  " + (e.to.empty() ? L"latest" : e.to);
        g_c.Text(v, Format(12.5f), D2D1::RectF(colVer, r.top, colTime - 8, r.bottom), g_t.subtext);
    }
    g_c.Text(ClockTime(e.time), Format(12.5f), D2D1::RectF(colTime, r.top, colStatus - 8, r.bottom), g_t.faint);
    StatusInfo st{L"Failed", g_t.bad};
    if (e.result == L"updated") st = {L"Updated", g_t.good};
    else if (e.result == L"restart") st = {L"Restart needed", g_t.warn};
    else if (e.result == L"uptodate") st = {L"Already current", g_t.info};
    else if (e.result == L"cancelled") st = {L"Stopped", g_t.faint};
    else if (e.result == L"skipped") st = {L"Skipped", g_t.faint};
    Pill(colStatus, cy - 10, st.label, st.color);
}

static void DrawSearch(const D2D1_RECT_F& r) {
    bool hot = IsHover(Hit::Search);
    g_c.FillRound(r, 6, g_searchFocus ? g_t.surface : (hot ? g_t.surfaceHover : Mix(g_t.surface, g_t.bg, 0.3f)));
    g_c.StrokeRound(r, 6, g_searchFocus ? g_t.accent : g_t.divider, g_searchFocus ? 1.5f : 1.0f);
    g_c.Glyph(icon::Search, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 6, r.top, r.left + 32, r.bottom), g_t.subtext);
    auto* f = Format(13);
    D2D1_RECT_F tr = D2D1::RectF(r.left + 34, r.top, r.right - 30, r.bottom);
    if (g_search.empty()) g_c.Text(g_searchFocus ? L"" : L"Search apps   Ctrl+K", f, tr, g_t.faint);
    else {
        if (g_searchFocus && g_searchAll) {
            float sw = std::min(MeasureWidth(g_search, f), tr.right - tr.left);
            g_c.FillRound(D2D1::RectF(tr.left - 1, r.top + 7, tr.left + sw + 1, r.bottom - 7), 2, Alpha(g_t.accent, 0.35f));
        }
        g_c.Text(g_search, f, tr, g_t.text);
    }
    if (g_searchFocus && g_caretOn) {
        float cx = tr.left + std::min(MeasureWidth(g_search, f), tr.right - tr.left - 2);
        g_c.Fill(D2D1::RectF(cx, r.top + 8, cx + 1.2f, r.bottom - 8), g_t.text);
    }
    AddHot(r, Hit::Search);
    if (!g_search.empty()) {
        D2D1_RECT_F cr = D2D1::RectF(r.right - 28, r.top + 4, r.right - 4, r.bottom - 4);
        if (IsHover(Hit::SearchClear)) g_c.FillRound(cr, 4, g_t.surfaceStrong);
        g_c.Glyph(icon::Cancel, Format(10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), cr, g_t.subtext);
        AddHot(cr, Hit::SearchClear);
    }
}

// A banner across the top of the list: icon, two lines of text, an optional button and progress
static float Banner(float left, float right, float y, wchar_t glyph, D2D1_COLOR_F color, const std::wstring& title, const std::wstring& text,
                    const std::wstring& button, Hit kind, float progress = -2) {
    float h = progress > -2 ? 74.0f : 64.0f;
    D2D1_RECT_F r = D2D1::RectF(left, y, right, y + h);
    g_c.FillRound(r, 10, Alpha(color, g_t.dark ? 0.12f : 0.09f));
    g_c.StrokeRound(r, 10, Alpha(color, 0.30f));
    g_c.Glyph(glyph, Format(18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 12, r.top + 10, r.left + 46, r.top + 44), color);
    float bw = button.empty() ? 0 : ButtonWidth(button, 0);
    float textRight = r.right - 18 - (bw ? bw + 12 : 0);
    g_c.Text(title, Format(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 54, r.top + 11, textRight, r.top + 31), g_t.text);
    g_c.Text(text, Format(12.5f), D2D1::RectF(r.left + 54, r.top + 31, textRight, r.top + 50), g_t.subtext);
    if (bw) Button(D2D1::RectF(r.right - 16 - bw, r.top + 16, r.right - 16, r.top + 48), button, 0, Btn::Secondary, kind);
    if (progress > -2) ProgressBar(D2D1::RectF(r.left + 54, r.bottom - 14, r.right - 18, r.bottom - 10), progress, color);
    return h + 10;
}

static const float kRowH = 64, kRowGap = 6, kSectionH = 40;

static float RowTop(int k) { return g_listTop + k * (kRowH + kRowGap) + (g_sectionAt >= 0 && k >= g_sectionAt ? kSectionH : 0); }

static void DrawList(float x0) {
    bool history = g_page == Page::History;
    if (history) ComputeHistoryVisible(); else ComputeVisible();
    float pad = 28;
    float left = x0 + pad, right = g_W - pad;
    float top = kHeader;
    g_viewH = g_H - top;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, top, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    float y0 = top + 6;
    float y = y0 - g_scroll;

    // banners (Updates and All apps)
    if (!history && (g_page == Page::Updates || g_page == Page::All)) {
        if (g_inv.wingetMissing || g_wingetBusy || (!g_wingetStatus.empty() && !g_inv.wingetOk)) {
            std::wstring t = g_wingetBusy ? g_wingetStatus : !g_wingetStatus.empty() ? g_wingetStatus
                                                                                     : L"Restock uses winget (App Installer) to update apps. It can set it up for you.";
            y += Banner(left, right, y, icon::Download, g_t.info, g_wingetBusy ? L"Setting up winget" : L"winget isn't installed", t,
                        g_wingetBusy ? L"" : L"Install winget", Hit::BannerWinget, g_wingetBusy ? -1.0f : -2.0f);
        } else if (!g_inv.wingetOk && !g_inv.wingetError.empty() && g_haveScan) {
            y += Banner(left, right, y, icon::Warning, g_t.warn, L"winget didn't answer", g_inv.wingetError + L" Showing apps from Windows' list only.",
                        L"Repair winget", Hit::BannerWinget);
        }
        if (int n = jobs::QueueSize()) {
            std::wstring cur = jobs::Running();
            PkgState* s = cur.empty() ? nullptr : FindPkg(cur);
            int done = std::min(g_queueDone + 1, std::max(g_queueTotal, 1));
            std::wstring title = L"Updating " + std::to_wstring(done) + L" of " + std::to_wstring(std::max(g_queueTotal, done));
            if (s) title += L" · " + s->p.name;
            std::wstring text = s ? s->opText : L"Starting…";
            if (n > 1) text += L"  ·  " + std::to_wstring(n - 1) + L" waiting";
            float pct = s && s->percent >= 0 ? s->shownPercent : -1;
            y += Banner(left, right, y, icon::Update, g_t.accent, title, text, n > 1 ? L"Stop after this one" : L"", Hit::BannerStop, pct);
        }
        if (!g_restartNeeded.empty()) {
            y += Banner(left, right, y, icon::Restart, g_t.warn, L"Restart your PC to finish",
                        JoinNames(g_restartNeeded) + (g_restartNeeded.size() == 1 ? L" finishes" : L" finish") + L" updating after a restart.",
                        L"Restart now", Hit::BannerRestart);
        }
    }
    g_listTop = y + g_scroll;

    if (history) {
        if (g_histVisible.empty()) {
            g_c.Glyph(icon::History, Format(34, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(left, y + 70, right, y + 120), g_t.faint);
            g_c.Text(g_search.empty() ? L"Nothing updated yet. Every update Restock runs is listed here." : L"No updates match \"" + g_search + L"\".",
                     Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(left, y + 124, right, y + 150), g_t.subtext);
            g_contentH = (y + 200) - (y0 - g_scroll);
        } else {
            std::wstring day;
            float rh = 54, gap = 6;
            for (int idx : g_histVisible) {
                const HistoryEntry& e = g_hist[idx];
                std::wstring d = DayLabel(e.time);
                if (d != day) {
                    day = d;
                    y += day == DayLabel(g_hist[g_histVisible[0]].time) ? 0 : 10;
                    if (y + 30 >= top && y <= g_H) g_c.Text(d, Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left + 2, y, right, y + 24), g_t.subtext);
                    y += 28;
                }
                D2D1_RECT_F r = D2D1::RectF(left, y, right, y + rh);
                if (r.bottom >= top && r.top <= g_H) DrawHistoryRow(e, r);
                y += rh + gap;
            }
            g_contentH = (y + 20) - (y0 - g_scroll);
        }
    } else if (g_visible.empty()) {
        bool searching = !g_search.empty();
        std::wstring msg = searching ? L"No apps match \"" + g_search + L"\"."
                         : g_page == Page::Updates ? (g_haveScan || !g_inv.pkgs.empty() ? L"Everything is up to date." : L"Checking your apps for updates…")
                         : g_page == Page::Hidden ? L"Nothing pinned or ignored. Use the \x22EF menu on an app to pin it, skip a version or ignore it."
                         : g_page == Page::SelfUpdating ? L"None of your apps update themselves."
                         : L"Looking for your apps…";
        wchar_t glyph = g_page == Page::Updates && !searching && (g_haveScan || !g_inv.pkgs.empty()) ? icon::Check : icon::Apps;
        g_c.Glyph(glyph, Format(34, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(left, y + 70, right, y + 120), g_t.faint);
        g_c.Text(msg, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(left, y + 124, right, y + 150), g_t.subtext);
        if (g_page == Page::Updates && !searching && g_inv.scannedAt)
            g_c.Text(L"Checked " + Ago(g_inv.scannedAt) + L" · F5 checks again", Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                     D2D1::RectF(left, y + 152, right, y + 172), g_t.faint);
        g_contentH = (y + 200) - (y0 - g_scroll);
    } else {
        for (size_t k = 0; k < g_visible.size(); ++k) {
            float ry = RowTop((int)k) - g_scroll;
            if ((int)k == g_sectionAt) {
                float sy = ry - kSectionH;
                g_c.Text(L"NOT IN UPDATE ALL", Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left + 2, sy + 12, right, sy + 32), g_t.subtext);
                g_c.Text(L"Pinned, skipped or self-updating. Update them one at a time if you want to.", Format(12),
                         D2D1::RectF(left + 160, sy + 12, right, sy + 32), g_t.faint);
            }
            D2D1_RECT_F r = D2D1::RectF(left, ry, right, ry + kRowH);
            if (r.bottom >= top && r.top <= g_H) DrawRow(g_visible[k], r, (int)k == g_focus);
        }
        g_contentH = RowTop((int)g_visible.size() - 1) + kRowH + 20 - (y0);
    }
    g_rt->PopAxisAlignedClip();

    // header (drawn after the content so it stays on top)
    g_c.Fill(D2D1::RectF(x0, 0, g_W, top), g_t.bg);
    if (g_scroll > 1) g_c.Line(x0, top - 0.5f, g_W, top - 0.5f, g_t.divider);
    g_c.Text(PageTitle(), Format(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, 16, right, 46), g_t.text);
    float bx = right;
    if (history) {
        if (!g_hist.empty()) {
            float w = ButtonWidth(L"Clear history", 0);
            Button(D2D1::RectF(bx - w, 22, bx, 54), L"Clear history", 0, Btn::Secondary, Hit::HistClear);
            bx -= w + 10;
        }
    } else {
        Button(D2D1::RectF(bx - 34, 22, bx, 54), L"", icon::Refresh, Btn::Secondary, Hit::Refresh, -1, 0, !jobs::Scanning());
        bx -= 44;
        int n = 0;
        for (auto& s : g_pkgs) n += Eligible(s) ? 1 : 0;
        if (n > 0 && (g_page == Page::Updates || g_page == Page::All)) {
            std::wstring label = L"Update all (" + std::to_wstring(n) + L")";
            float w = ButtonWidth(label, icon::Update);
            Button(D2D1::RectF(bx - w, 22, bx, 54), label, icon::Update, Btn::Primary, Hit::UpdateAll);
            bx -= w + 10;
        }
    }
    float sw = std::min(280.0f, bx - left - 170);
    if (sw > 120) DrawSearch(D2D1::RectF(bx - sw, 22, bx, 54));
    float subRight = sw > 120 ? bx - sw - 12 : bx;
    g_c.Text(PageSubtitle(), Format(12.5f), D2D1::RectF(left, 44, subRight, 62), g_t.subtext);
}

// ---------------------------------------------------------------- themes
static std::vector<ThemeDef> AllThemes() {
    std::vector<ThemeDef> all = BuiltinThemes();
    all.insert(all.end(), g_customThemes.begin(), g_customThemes.end());
    return all;
}

static std::wstring HexColor(uint32_t c) {
    wchar_t b[16];
    swprintf(b, 16, L"#%06X", c & 0xFFFFFF);
    return b;
}

static bool PickCustomAccent() {
    static COLORREF custom[16] = {};
    uint32_t cur = ResolveAccent(FindTheme(g_cfg.theme, g_customThemes), g_cfg.accent);
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = g_hwnd;
    cc.rgbResult = RGB((cur >> 16) & 0xFF, (cur >> 8) & 0xFF, cur & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    g_cfg.accent = HexColor((GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult));
    return true;
}

static void ExportTheme() {
    ThemeDef cur = FindTheme(g_cfg.theme, g_customThemes);
    if (cur.followWindows) cur = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
    std::wstring base = cur.custom ? cur.name : cur.name + L" (copy)";
    for (auto& ch : base) if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'-';
    EnsureDir(ThemesDir());
    std::wstring path = ThemesDir() + L"\\" + base + L".json";
    for (int n = 2; FileExists(path) && !cur.custom; ++n) path = ThemesDir() + L"\\" + base + L" " + std::to_wstring(n) + L".json";
    if (!WriteTextFileAtomic(path, ThemeToJson(cur, g_cfg.accent))) { Toast(L"Couldn't save the theme file.", true); return; }
    g_customThemes = LoadCustomThemes();
    Toast(L"Saved " + PathName(path) + L". Edit it, then press Reload.");
    ShellExecuteW(g_hwnd, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// A miniature Restock window drawn in the theme's colors
static void DrawThemePreview(const ThemeDef& def0, const D2D1_RECT_F& r, int index, bool selected) {
    ThemeDef def = def0;
    if (def.followWindows) def = BuiltinThemes()[SystemUsesLightTheme() ? 1 : 0];
    std::wstring accentSetting = selected ? g_cfg.accent : L"theme";
    Theme pt = MakeTheme(def, accentSetting);
    bool hot = IsHover(Hit::ThemePick, -1, index);
    D2D1_RECT_F card = r;
    g_c.FillRound(card, 10, hot ? g_t.surfaceHover : g_t.surface);
    D2D1_RECT_F pv = D2D1::RectF(r.left + 10, r.top + 10, r.right - 10, r.bottom - 50);
    ID2D1RoundedRectangleGeometry* clip = nullptr;
    d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(pv, 7, 7), &clip);
    if (!g_previewLayer) g_rt->CreateLayer(&g_previewLayer);
    ID2D1Layer* layer = g_previewLayer;
    if (clip && layer) g_rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
    if (def.backdrop == L"acrylic" || def.surfaceOpacity < 1.0f) {
        ID2D1GradientStopCollection* stops = nullptr;
        D2D1_GRADIENT_STOP gs[3] = {{0, Rgb(0x3A5BD9)}, {0.55f, Rgb(0x8E4FD6)}, {1, Rgb(0xE0679A)}};
        g_rt->CreateGradientStopCollection(gs, 3, &stops);
        ID2D1LinearGradientBrush* lg = nullptr;
        if (stops) g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(pv.left, pv.top), D2D1::Point2F(pv.right, pv.bottom)), stops, &lg);
        if (lg) { g_rt->FillRectangle(pv, lg); lg->Release(); }
        if (stops) stops->Release();
        g_c.Fill(pv, Rgb(def.bg, 1.0f - def.transparency));
    } else {
        g_c.Fill(pv, Rgb(def.bg));
    }
    float w = pv.right - pv.left, h = pv.bottom - pv.top;
    D2D1_RECT_F sb = D2D1::RectF(pv.left, pv.top, pv.left + w * 0.27f, pv.bottom);
    g_c.Fill(sb, def.backdrop == L"acrylic" ? Rgb(def.sidebar, 0.35f) : Rgb(def.sidebar));
    g_c.FillRound(D2D1::RectF(sb.left + 6, pv.top + 7, sb.left + 14, pv.top + 15), 2, pt.accent);
    for (int k = 0; k < 4; ++k) {
        float ly = pv.top + 24 + k * 11;
        if (k == 0) g_c.FillRound(D2D1::RectF(sb.left + 4, ly - 3, sb.right - 4, ly + 6), 2, Alpha(pt.text, 0.10f));
        g_c.FillRound(D2D1::RectF(sb.left + 8, ly, sb.left + 8 + (sb.right - sb.left - 18) * (k == 0 ? 0.8f : 0.55f + 0.1f * (k % 2)), ly + 3), 1.5f, k == 0 ? pt.text : pt.subtext);
    }
    // content: a title and three update rows
    float cx = sb.right + 8;
    g_c.FillRound(D2D1::RectF(cx, pv.top + 8, cx + w * 0.22f, pv.top + 12), 2, pt.text);
    float rowH = (h - 26) / 3 - 4;
    for (int k = 0; k < 3; ++k) {
        D2D1_RECT_F c = D2D1::RectF(cx, pv.top + 20 + k * (rowH + 4), pv.right - 8, pv.top + 20 + k * (rowH + 4) + rowH);
        g_c.FillRound(c, 3, def.surfaceOpacity < 1.0f ? Rgb(def.surface, def.surfaceOpacity * 1.8f) : pt.surface);
        float my = (c.top + c.bottom) / 2;
        g_c.FillRound(D2D1::RectF(c.left + 4, my - 3.5f, c.left + 11, my + 3.5f), 2, Alpha(pt.accent, k == 1 ? 0.5f : 0.9f));
        g_c.FillRound(D2D1::RectF(c.left + 15, my - 1.5f, c.left + 15 + (c.right - c.left) * 0.32f, my + 1.5f), 1.2f, pt.text);
        g_c.FillRound(D2D1::RectF(c.right - 24, my - 3, c.right - 5, my + 3), 2, k == 0 ? pt.accent : pt.surfaceStrong);
    }
    if (layer && clip) g_rt->PopLayer();
    if (clip) clip->Release();
    g_c.StrokeRound(pv, 7, Alpha(g_t.text, 0.10f));
    g_c.Text(def0.name, Format(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 14, r.bottom - 44, r.right - 40, r.bottom - 24), g_t.text);
    g_c.Text(def0.description, Format(11.5f), D2D1::RectF(r.left + 14, r.bottom - 25, r.right - 12, r.bottom - 8), g_t.subtext);
    if (def0.custom) Pill(r.right - 12, r.bottom - 43, L"Custom", g_t.info, true);
    if (selected) {
        g_c.StrokeRound(card, 10, g_t.accent, 2);
        g_c.Circle(r.right - 22, r.top + 22, 10, g_t.accent);
        g_c.Glyph(icon::Check, Format(10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
                  D2D1::RectF(r.right - 32, r.top + 12, r.right - 12, r.top + 32), g_t.onAccent);
    } else if (hot) {
        g_c.StrokeRound(card, 10, Alpha(g_t.accent, 0.5f));
    }
    AddHot(card, Hit::ThemePick, -1, index);
}

static void DrawThemes(float x0) {
    float pad = 32;
    float left = x0 + pad, right = std::min(g_W - pad, x0 + pad + 1100);
    g_viewH = g_H;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, 0, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = 22 - g_scroll;
    g_c.Text(L"Themes", Format(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 32), g_t.text);
    g_c.Text(L"Click a theme to try it. It changes right away.", Format(12.5f), D2D1::RectF(left, y + 30, right, y + 48), g_t.subtext);
    y += 66;
    auto all = AllThemes();
    float gap = 14, minW = 200, avail = right - left;
    int cols = std::max(1, (int)((avail + gap) / (minW + gap)));
    float cw = (avail - gap * (cols - 1)) / cols, ch = std::min(190.0f, cw * 0.62f + 50);
    for (size_t k = 0; k < all.size(); ++k) {
        int c = (int)(k % cols), row = (int)(k / cols);
        D2D1_RECT_F r = D2D1::RectF(left + c * (cw + gap), y + row * (ch + gap), left + c * (cw + gap) + cw, y + row * (ch + gap) + ch);
        if (r.bottom > 0 && r.top < g_H) DrawThemePreview(all[k], r, (int)k, all[k].id == g_cfg.theme);
    }
    y += ((int)all.size() + cols - 1) / cols * (ch + gap) + 18;

    g_c.Text(L"ACCENT COLOR", Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 20), g_t.subtext);
    y += 28;
    D2D1_RECT_F box = D2D1::RectF(left, y, right, y + 64);
    g_c.FillRound(box, 10, g_t.surface);
    float ax = left + 16, ay = y + 16;
    ThemeDef cur = FindTheme(g_cfg.theme, g_customThemes);
    auto chip = [&](const std::wstring& label, uint32_t color, int arg, bool sel) {
        auto* f = Format(12.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
        float w = MeasureWidth(label, f) + 42;
        D2D1_RECT_F r = D2D1::RectF(ax, ay, ax + w, ay + 32);
        g_c.FillRound(r, 16, sel ? Alpha(g_t.accent, 0.18f) : (IsHover(Hit::AccentPick, -1, arg) ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.4f)));
        if (sel) g_c.StrokeRound(r, 16, g_t.accent, 1.5f);
        g_c.Circle(r.left + 16, ay + 16, 7, Rgb(color));
        g_c.Text(label, f, D2D1::RectF(r.left + 29, ay, r.right - 8, ay + 32), g_t.text);
        AddHot(r, Hit::AccentPick, -1, arg);
        ax += w + 8;
    };
    chip(L"Theme's own", ResolveAccent(cur, L"theme"), 0, g_cfg.accent == L"theme");
    chip(L"Windows", SystemAccent(), 1, g_cfg.accent == L"windows");
    ax += 6;
    bool customSel = g_cfg.accent.size() == 7 && g_cfg.accent[0] == L'#';
    for (int k = 0; k < kAccentSwatchCount; ++k) {
        bool sel = IEquals(g_cfg.accent, HexColor(kAccentSwatches[k]));
        if (sel) customSel = false;
        float cxx = ax + 16;
        if (cxx + 16 > right - 110) break;
        if (sel) g_c.Circle(cxx, ay + 16, 15, g_t.text);
        else if (IsHover(Hit::AccentPick, -1, 2 + k)) g_c.Circle(cxx, ay + 16, 15, Alpha(g_t.text, 0.3f));
        g_c.Circle(cxx, ay + 16, sel ? 12.5f : 13, Rgb(kAccentSwatches[k]));
        AddHot(D2D1::RectF(ax, ay, ax + 32, ay + 32), Hit::AccentPick, -1, 2 + k);
        ax += 38;
    }
    ax += 4;
    chip(customSel ? g_cfg.accent : L"Custom…", customSel ? ResolveAccent(cur, g_cfg.accent) : 0x888888, 100, customSel);
    y += 64 + 26;

    g_c.Text(L"YOUR THEMES", Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 20), g_t.subtext);
    y += 28;
    D2D1_RECT_F b2 = D2D1::RectF(left, y, right, y + 64);
    g_c.FillRound(b2, 10, g_t.surface);
    g_c.Text(L"Theme files", Format(14), D2D1::RectF(left + 18, y + 12, right - 420, y + 32), g_t.text);
    g_c.Text(L"Same format as Hangar. Themes in Restock's or Hangar's themes folder appear above.",
             Format(12), D2D1::RectF(left + 18, y + 32, right - 420, y + 50), g_t.subtext);
    float bx = right - 18;
    auto btn = [&](const std::wstring& l, wchar_t g, Hit kind) {
        float w = ButtonWidth(l, g);
        Button(D2D1::RectF(bx - w, y + 16, bx, y + 48), l, g, Btn::Secondary, kind);
        bx -= w + 8;
    };
    btn(L"Reload", icon::Refresh, Hit::ThemeReload);
    btn(L"Open folder", icon::Folder, Hit::ThemeFolder);
    btn(L"Save as file", icon::Download, Hit::ThemeExport);
    y += 64 + 30;
    g_contentH = y + g_scroll;
    g_rt->PopAxisAlignedClip();
}

// ---------------------------------------------------------------- settings
static void Segmented(float x, float y, const std::vector<std::wstring>& opts, int sel, Hit kind, int index) {
    float cx = x;
    for (size_t k = 0; k < opts.size(); ++k) {
        float w = MeasureWidth(opts[k], Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER)) + 30;
        D2D1_RECT_F r = D2D1::RectF(cx, y, cx + w, y + 32);
        bool s = (int)k == sel;
        g_c.FillRound(r, 6, s ? Alpha(g_t.accent, 0.18f) : (IsHover(kind, index, (int)k) ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.3f)));
        if (s) g_c.StrokeRound(r, 6, Alpha(g_t.accent, 0.6f));
        g_c.Text(opts[k], Format(13, s ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), r, s ? g_t.text : g_t.subtext);
        AddHot(r, kind, index, (int)k);
        cx += w + 6;
    }
}

static float SegmentedWidth(const std::vector<std::wstring>& opts) {
    float w = 0;
    for (auto& o : opts) w += MeasureWidth(o, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER)) + 36;
    return w - 6;
}

static void DrawSettings(float x0) {
    float pad = 32;
    float left = x0 + pad, right = std::min(g_W - pad, x0 + pad + 860);
    g_viewH = g_H;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, 0, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = 22 - g_scroll;
    g_c.Text(L"Settings", Format(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 32), g_t.text);
    y += 52;
    auto section = [&](const std::wstring& title, int rows) {
        g_c.Text(title, Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 20), g_t.subtext);
        y += 26;
        D2D1_RECT_F r = D2D1::RectF(left, y, right, y + 64.0f * rows);
        g_c.FillRound(r, 10, g_t.surface);
        for (int k = 1; k < rows; ++k) g_c.Line(left + 18, r.top + 64.0f * k, right - 18, r.top + 64.0f * k, g_t.divider);
        return r;
    };
    auto rowLabel = [&](const D2D1_RECT_F& s, int row, const std::wstring& t, const std::wstring& sub, float controlW = 380) {
        float ry = s.top + 64.0f * row + 4;
        g_c.Text(t, Format(14), D2D1::RectF(left + 18, ry + 8, right - controlW, ry + 28), g_t.text);
        if (!sub.empty()) g_c.Text(sub, Format(12), D2D1::RectF(left + 18, ry + 28, right - controlW, ry + 46), g_t.subtext);
    };
    auto seg = [&](const D2D1_RECT_F& s, int row, const std::vector<std::wstring>& opts, int sel, int setting) {
        Segmented(right - 18 - SegmentedWidth(opts), s.top + 64.0f * row + 16, opts, sel, Hit::Seg, setting);
    };
    auto onOff = [&](const D2D1_RECT_F& s, int row, bool on, int setting) { seg(s, row, {L"On", L"Off"}, on ? 0 : 1, setting); };
    auto button = [&](const D2D1_RECT_F& s, int row, const std::wstring& label, wchar_t glyph, Hit kind, Btn style = Btn::Secondary, bool enabled = true) {
        float w = std::max(122.0f, ButtonWidth(label, glyph));
        Button(D2D1::RectF(right - 18 - w, s.top + 64.0f * row + 16, right - 18, s.top + 64.0f * row + 48), label, glyph, style, kind, -1, 0, enabled);
    };

    // schedule
    D2D1_RECT_F s1 = section(L"SCHEDULE", 5);
    rowLabel(s1, 0, L"Check for updates", L"Restock checks from the tray icon. Last check: " + Ago(g_inv.scannedAt));
    seg(s1, 0, {L"Off", L"Daily", L"Weekly"}, g_cfg.schedule == L"off" ? 0 : g_cfg.schedule == L"weekly" ? 2 : 1, SSchedule);
    rowLabel(s1, 1, L"Notify me when updates are waiting", L"A Windows notification, once per set of updates");
    onOff(s1, 1, g_cfg.notify, SNotify);
    rowLabel(s1, 2, L"Update everything automatically at night", L"2 to 5 AM. Skips anything open, pinned, skipped or installed for all users");
    onOff(s1, 2, g_cfg.nightly, SNightly);
    rowLabel(s1, 3, L"Keep running in the tray", L"Closing the window leaves Restock in the tray so the schedule can run");
    onOff(s1, 3, g_cfg.tray, STray);
    rowLabel(s1, 4, L"Start with Windows", L"Opens quietly in the tray when you sign in");
    onOff(s1, 4, g_cfg.startup, SStartup);
    y = s1.bottom + 26;

    // which apps
    D2D1_RECT_F s2 = section(L"UPDATES", 4);
    rowLabel(s2, 0, L"Include self-updating apps", L"Chrome, Discord, Steam and others update themselves; Restock leaves them alone");
    onOff(s2, 0, g_cfg.includeSelfUpdating, SSelfUpdating);
    rowLabel(s2, 1, L"Include apps with an unknown version", L"winget can't tell which version some apps are; it can still update them");
    onOff(s2, 1, g_cfg.includeUnknown, SUnknown);
    rowLabel(s2, 2, L"Reopen apps after updating", L"Apps Restock closed to update open again afterwards");
    onOff(s2, 2, g_cfg.reopen, SReopen);
    size_t managed = g_cfg.pinned.size() + g_cfg.ignored.size() + g_cfg.skipped.size();
    rowLabel(s2, 3, L"Pinned, ignored and skipped", managed ? std::to_wstring(CountPage(Page::Hidden)) + L" apps" : L"None yet. Use the \x22EF menu on any app");
    button(s2, 3, L"Manage", 0, Hit::SetManage);
    y = s2.bottom + 26;

    // appearance
    D2D1_RECT_F s3 = section(L"APPEARANCE", 1);
    {
        ThemeDef cur = FindTheme(g_cfg.theme, g_customThemes);
        rowLabel(s3, 0, L"Theme", cur.name + L" · " + cur.description);
        button(s3, 0, L"Browse themes", icon::Color, Hit::ThemeBrowse);
    }
    y = s3.bottom + 26;

    // Restock's own updates
    int selfRows = 3 + (g_prevVersions.empty() ? 0 : 1);
    D2D1_RECT_F s4 = section(L"RESTOCK UPDATES", selfRows);
    {
        std::wstring status;
        switch (g_self.kind) {
            case SelfUpdateState::Checking: status = L"Checking…"; break;
            case SelfUpdateState::UpToDate: status = std::wstring(L"Up to date · ") + RESTOCK_VERSION; break;
            case SelfUpdateState::Downloading: status = L"Downloading " + g_self.version + (g_self.percent >= 0 ? L" · " + std::to_wstring(g_self.percent) + L"%" : L""); break;
            case SelfUpdateState::Ready: status = L"Restock " + g_self.version + L" is downloaded and verified"; break;
            case SelfUpdateState::Error: status = g_self.message; break;
            default: status = std::wstring(L"Version ") + RESTOCK_VERSION; break;
        }
        rowLabel(s4, 0, std::wstring(L"Restock ") + RESTOCK_VERSION, status);
        if (g_self.kind == SelfUpdateState::Ready) button(s4, 0, L"Restart to update", icon::Update, Hit::SelfRestart, Btn::Primary);
        else button(s4, 0, L"Check for updates now", icon::Refresh, Hit::SelfCheck, Btn::Secondary,
                    g_self.kind != SelfUpdateState::Checking && g_self.kind != SelfUpdateState::Downloading);
        rowLabel(s4, 1, L"Channel", L"Beta gets new Restock features first");
        seg(s4, 1, {L"Stable", L"Beta"}, g_cfg.selfBeta ? 1 : 0, SSelfChannel);
        rowLabel(s4, 2, L"Install Restock updates automatically", L"Only when nothing is updating and you're not using Restock");
        onOff(s4, 2, g_cfg.selfAuto, SSelfAuto);
        if (!g_prevVersions.empty()) {
            rowLabel(s4, 3, L"Previous versions", L"Go back if a new Restock gives you trouble");
            float bx = right - 18;
            for (size_t k = 0; k < g_prevVersions.size() && k < 2; ++k) {
                std::wstring l = L"Roll back to " + g_prevVersions[k].version;
                float w = ButtonWidth(l, 0);
                Button(D2D1::RectF(bx - w, s4.top + 64 * 3 + 16, bx, s4.top + 64 * 3 + 48), l, 0, Btn::Secondary, Hit::SelfRollback, -1, (int)k);
                bx -= w + 8;
            }
        }
    }
    y = s4.bottom + 26;

    // winget and the suite
    D2D1_RECT_F s5 = section(L"WINGET AND YOUR SUITE", 2);
    {
        std::wstring wv = g_inv.wingetVersion.empty() ? (g_inv.wingetMissing ? L"Not installed" : L"Version unknown") : g_inv.wingetVersion;
        rowLabel(s5, 0, L"winget", wv + (g_wingetBusy ? L" · " + g_wingetStatus : L""));
        button(s5, 0, g_inv.wingetMissing ? L"Install winget" : L"Repair winget", 0, Hit::SetWingetRepair, Btn::Secondary, !g_wingetBusy);
        rowLabel(s5, 1, L"larosa023 apps", g_suiteCount ? std::to_wstring(g_suiteCount) + L" of your suite apps are left to Hangar, which keeps them current"
                                                        : L"Your suite apps are left to Hangar (from its app list)");
        button(s5, 1, L"Open Hangar", icon::Link, Hit::OpenHangar);
    }
    y = s5.bottom + 26;

    D2D1_RECT_F s6 = section(L"ADVANCED", 2);
    rowLabel(s6, 0, L"Data folder", DataDir());
    button(s6, 0, L"Open", icon::Folder, Hit::SetOpenData);
    rowLabel(s6, 1, L"Log", L"restock.log, for troubleshooting (winget's output for failed updates is in it)");
    button(s6, 1, L"Open log", 0, Hit::SetOpenLog);
    y = s6.bottom + 26;

    D2D1_RECT_F s7 = section(L"ABOUT", 1);
    rowLabel(s7, 0, std::wstring(L"Restock ") + RESTOCK_VERSION + (IsPortable() ? L" (portable)" : L""), L"larosa023 · no accounts, no telemetry");
    {
        float w = 190;
        Button(D2D1::RectF(right - 18 - w, s7.top + 16, right - 18, s7.top + 48), L"github.com/larosa023/Restock", 0, Btn::Subtle, Hit::SetRepo);
    }
    y = s7.bottom + 26;
    g_contentH = y + g_scroll + 10;
    g_rt->PopAxisAlignedClip();
}

// ---------------------------------------------------------------- toast and what's new
static void DrawToast() {
    if (g_toastAnim <= 0.01f || g_toast.empty()) return;
    auto* f = Format(13.5f);
    float w = std::min(g_W - kSidebar - 60, MeasureWidth(g_toast, f) + 70);
    float cx = kSidebar + (g_W - kSidebar) / 2;
    float y = g_H - 20 - 44 * g_toastAnim;
    D2D1_RECT_F r = D2D1::RectF(cx - w / 2, y, cx + w / 2, y + 40);
    g_c.FillRound(r, 8, Alpha(g_t.dark ? Rgb(0x2E2E36) : Rgb(0x26262C), g_toastAnim));
    g_c.Glyph(g_toastBad ? icon::Warning : icon::Check, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
              D2D1::RectF(r.left + 8, r.top, r.left + 36, r.bottom), Alpha(g_toastBad ? Rgb(0xFF8A8A) : Rgb(0x7BE0A6), g_toastAnim));
    g_c.Text(g_toast, f, D2D1::RectF(r.left + 38, r.top, r.right - 30, r.bottom), Alpha(Rgb(0xF4F4F6), g_toastAnim));
    D2D1_RECT_F x = D2D1::RectF(r.right - 30, r.top + 8, r.right - 6, r.bottom - 8);
    g_c.Glyph(icon::Cancel, Format(9, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), x, Alpha(Rgb(0xB0B0BA), g_toastAnim));
    AddHot(r, Hit::ToastClose);
}

static void DrawWhatsNew() {
    if (g_whatsNew.empty()) return;
    g_hot.clear();   // the overlay is modal
    g_c.Fill(D2D1::RectF(0, 0, g_W, g_H), Rgb(0x000000, 0.45f));
    AddHot(D2D1::RectF(0, 0, g_W, g_H), Hit::WhatsNewClose);
    float w = std::min(560.0f, g_W - 80);
    auto* f = Format(13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    float th = std::min(MeasureHeight(g_whatsNew, f, w - 56), g_H - 260);
    float h = th + 150;
    D2D1_RECT_F r = D2D1::RectF((g_W - w) / 2, (g_H - h) / 2, (g_W + w) / 2, (g_H + h) / 2);
    g_c.FillRound(r, 12, g_t.dark ? Rgb(0x1F1F25) : Rgb(0xFFFFFF));
    g_c.StrokeRound(r, 12, g_t.divider);
    AddHot(r, Hit::None);
    g_c.Glyph(icon::Star, Format(18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 22, r.top + 22, r.left + 50, r.top + 50), g_t.accent);
    g_c.Text(L"Restock updated to " + g_whatsNewVersion, Format(19, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 56, r.top + 20, r.right - 24, r.top + 52), g_t.text);
    g_c.Text(L"You're right where you left off.", Format(12.5f), D2D1::RectF(r.left + 56, r.top + 50, r.right - 24, r.top + 70), g_t.subtext);
    g_rt->PushAxisAlignedClip(D2D1::RectF(r.left + 28, r.top + 84, r.right - 28, r.top + 84 + th), D2D1_ANTIALIAS_MODE_ALIASED);
    g_c.Text(g_whatsNew, f, D2D1::RectF(r.left + 28, r.top + 84, r.right - 28, r.top + 84 + th + 4), g_t.subtext);
    g_rt->PopAxisAlignedClip();
    Button(D2D1::RectF(r.right - 120, r.bottom - 50, r.right - 24, r.bottom - 18), L"Got it", 0, Btn::Primary, Hit::WhatsNewClose);
}

// ---------------------------------------------------------------- paint
static bool IsListPage(Page p) { return p == Page::Updates || p == Page::All || p == Page::SelfUpdating || p == Page::Hidden || p == Page::History; }

static void ClampScroll() {
    float maxS = std::max(0.0f, g_contentH - g_viewH);
    g_scrollTarget = std::clamp(g_scrollTarget, 0.0f, maxS);
    g_scroll = std::clamp(g_scroll, 0.0f, maxS);
}

static float g_trackTop = 0, g_trackH = 0, g_thumbTop = 0, g_thumbH = 0;
static bool g_dragThumb = false;
static float g_dragGrab = 0;

static void DrawScrollbar() {
    float top = IsListPage(g_page) ? kHeader : 0;
    if (g_contentH <= g_viewH + 1) { g_thumbH = 0; return; }
    g_trackTop = top + 4;
    g_trackH = g_viewH - 8;
    g_thumbH = std::max(40.0f, g_trackH * g_viewH / g_contentH);
    float t = g_scroll / (g_contentH - g_viewH);
    g_thumbTop = g_trackTop + (g_trackH - g_thumbH) * t;
    bool active = g_dragThumb || IsHover(Hit::ScrollThumb) || IsHover(Hit::ScrollTrack);
    float w = active ? 8.0f : 4.0f;
    if (active) g_c.FillRound(D2D1::RectF(g_W - 12, g_trackTop, g_W - 2, g_trackTop + g_trackH), 5, Alpha(g_t.text, 0.05f));
    D2D1_COLOR_F thumb = g_t.dark ? Rgb(0xFFFFFF, active ? 0.34f : 0.18f) : Rgb(0x000000, active ? 0.36f : 0.2f);
    g_c.FillRound(D2D1::RectF(g_W - 3 - w, g_thumbTop, g_W - 3, g_thumbTop + g_thumbH), w / 2, thumb);
    AddHot(D2D1::RectF(g_W - 14, g_trackTop, g_W, g_trackTop + g_trackH), Hit::ScrollTrack);
    AddHot(D2D1::RectF(g_W - 14, g_thumbTop, g_W, g_thumbTop + g_thumbH), Hit::ScrollThumb);
}

static void DragThumbTo(float y) {
    float range = g_trackH - g_thumbH;
    if (range <= 0) return;
    float t = std::clamp((y - g_dragGrab - g_trackTop) / range, 0.0f, 1.0f);
    g_scroll = g_scrollTarget = t * (g_contentH - g_viewH);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// RESTOCK_PERF=1 logs frame times; time to first frame is always logged
static bool g_perf = false;
static LARGE_INTEGER g_qpf, g_startQpc;
static double g_frameMsSum = 0, g_frameMsMax = 0;
static int g_frames = 0;
static bool g_firstFrameLogged = false;

static double QpcMs(LARGE_INTEGER a, LARGE_INTEGER b) { return (b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart; }

static void PerfFrame(LARGE_INTEGER t0) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    double ms = QpcMs(t0, t1);
    if (!g_firstFrameLogged) {
        g_firstFrameLogged = true;
        FILETIME created, exited, kernel, user, now;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        GetSystemTimePreciseAsFileTime(&now);
        double sinceLaunch = ((((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) -
                              (((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime)) / 10000.0;
        Log(L"perf: first frame %.0f ms after launch (frame %.1f ms)", sinceLaunch, ms);
    }
    g_frameMsSum += ms;
    g_frameMsMax = std::max(g_frameMsMax, ms);
    if (!g_perf) return;
    if (++g_frames == 120) {
        PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
        Log(L"perf: page=%d avg %.2f ms, max %.2f ms over 120 frames; working set %.1f MB",
            (int)g_page, g_frameMsSum / 120, g_frameMsMax, pmc.WorkingSetSize / 1048576.0);
        g_frames = 0;
        g_frameMsSum = g_frameMsMax = 0;
    }
}

static void Paint() {
    LARGE_INTEGER perfT0{};
    if (g_perf || !g_firstFrameLogged) QueryPerformanceCounter(&perfT0);
    if (!EnsureDevice()) return;
    D2D1_SIZE_F sz = g_rt->GetSize();
    g_W = sz.width;
    g_H = sz.height;
    g_c.rt = g_rt;
    g_c.br = g_br;
    g_hot.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(g_t.bg);
    float x0 = kSidebar;
    float slide = (1.0f - g_pageAnim) * 14;
    if (slide > 0.05f) g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, slide));
    if (IsListPage(g_page)) DrawList(x0);
    else if (g_page == Page::Themes) DrawThemes(x0);
    else DrawSettings(x0);
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (g_pageAnim < 0.999f) { D2D1_COLOR_F o = g_t.bg; o.a = (1.0f - g_pageAnim) * 0.85f; g_c.Fill(D2D1::RectF(x0, 0, g_W, g_H), o); }
    DrawScrollbar();
    DrawSidebar();
    DrawToast();
    DrawWhatsNew();
    HRESULT hr = g_rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) DiscardDevice();
    gfx::FrameDone();
    ClampScroll();
    if (g_perf || !g_firstFrameLogged) PerfFrame(perfT0);
}

// ---------------------------------------------------------------- animation
static bool g_animations = true;
static LARGE_INTEGER g_lastAnimQpc{};

static void ReadAnimationSetting() {
    BOOL on = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) g_animations = on != FALSE;
}

static bool Ease(float& v, float target, float rate, float dtFrames, float snap) {
    if (!g_animations || std::fabs(target - v) <= snap) { v = target; return false; }
    v += (target - v) * (1.0f - std::pow(1.0f - rate, dtFrames));
    return true;
}

static bool Animate() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    float dt = g_lastAnimQpc.QuadPart ? (float)QpcMs(g_lastAnimQpc, now) : 16.67f;
    g_lastAnimQpc = now;
    float frames = std::clamp(dt / 16.67f, 0.25f, 4.0f);
    bool more = false;
    more |= Ease(g_scroll, g_scrollTarget, 0.26f, frames, 0.5f);
    for (size_t i = 0; i < g_pkgs.size(); ++i) {
        PkgState& s = g_pkgs[i];
        bool hot = (g_hover.kind == Hit::Row || g_hover.kind == Hit::RowAction || g_hover.kind == Hit::RowMore) && g_hover.index == (int)i;
        more |= Ease(s.hover, hot ? 1.0f : 0.0f, 0.28f, frames, 0.01f);
        if (s.queued && s.qstate != QueueState::Waiting) {
            if (s.percent < 0) more = true;
            else more |= Ease(s.shownPercent, (float)s.percent, 0.22f, frames, 0.2f);
        }
    }
    if (g_wingetBusy) more = true;
    if (g_pageAnim < 1.0f) {
        g_pageAnim = g_animations ? std::min(1.0f, g_pageAnim + 0.12f * frames) : 1.0f;
        more |= g_pageAnim < 1.0f;
    }
    float toastTarget = (!g_toast.empty() && GetTickCount() - g_toastAt < 60000) ? 1.0f : 0.0f;
    if (!Ease(g_toastAnim, toastTarget, 0.25f, frames, 0.01f) && toastTarget == 0) g_toast.clear();
    else if (g_toastAnim != toastTarget) more = true;
    if (!more) g_lastAnimQpc.QuadPart = 0;
    return more;
}

// ---------------------------------------------------------------- navigation
static void Navigate(Page p) {
    g_savedScroll[g_page] = g_scrollTarget;
    if (IsListPage(g_page)) g_lastList = g_page;
    if (p == Page::History) { g_hist = history::Load(); std::reverse(g_hist.begin(), g_hist.end()); }
    if (p == Page::Settings) g_prevVersions = selfupdate::PreviousVersions();
    if (g_page != p) g_focus = -1;
    g_page = p;
    if (IsListPage(p)) g_cfg.page = PageId(p);
    g_scroll = g_scrollTarget = IsListPage(p) ? g_savedScroll[p] : 0;
    g_pageAnim = 0.0f;
    g_searchFocus = false;
    StartAnim();
}

static void ShowNav(const std::wstring& id) {
    if (id == L"hangar") { OpenHangar(); return; }
    Navigate(NavPage(id));
}

static void EnsureFocusVisible() {
    if (g_focus < 0 || g_page == Page::History) return;
    float top = RowTop(g_focus) - (kHeader + 6), bottom = top + kRowH + kRowGap;
    if (top < g_scrollTarget) g_scrollTarget = top - (g_focus == g_sectionAt ? kSectionH : 0);
    else if (bottom > g_scrollTarget + g_viewH - 20) g_scrollTarget = bottom - g_viewH + 20;
    ClampScroll();
    StartAnim();
}

// ---------------------------------------------------------------- input
static HotRect HitAt(float x, float y) {
    for (auto it = g_hot.rbegin(); it != g_hot.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) {
            // anything scrolled under the header isn't clickable
            if (IsListPage(g_page) && y < kHeader && x > kSidebar &&
                (it->kind == Hit::Row || it->kind == Hit::RowAction || it->kind == Hit::RowMore || it->kind == Hit::BannerStop ||
                 it->kind == Hit::BannerRestart || it->kind == Hit::BannerWinget)) continue;
            return *it;
        }
    return {{0, 0, 0, 0}, Hit::None, -1, 0, false};
}

// ---------------------------------------------------------------- the window comes and goes (tray)
static void SaveWindowPlacement();

static void HideToTray() {
    SaveWindowPlacement();
    ConfigSave();
    ShowWindow(g_hwnd, SW_HIDE);
    KillTimer(g_hwnd, TIMER_PROCS);
    KillTimer(g_hwnd, TIMER_ANIM);
    g_animTimer = false;
    // nothing to draw: hand the GPU and memory back
    DiscardDevice();
    for (auto& s : g_pkgs) { std::vector<uint8_t>().swap(s.iconPixels); s.iconRequested = false; }
    gfx::ClearTextCaches();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    if (!g_cfg.trayHintShown) {
        g_cfg.trayHintShown = true;
        ConfigSave();
        tray::Notify(L"Restock is still running", L"It checks for updates from here. Right-click the icon to quit.");
    }
}

static void ShowFromTray(Page page = (Page)-1) {
    if (page != (Page)-1 && g_page != page) Navigate(page);
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, g_cfg.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    else if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    SetTimer(g_hwnd, TIMER_PROCS, 1500, nullptr);
    // results older than 10 minutes get a fresh look
    if (!jobs::Scanning() && (long long)time(nullptr) - g_inv.scannedAt > 10 * 60 && jobs::QueueSize() == 0) RescanSoon();
    Invalidate();
}

static bool g_quitting = false;

static void Quit(bool force) {
    if (!force) {
        if (int n = jobs::QueueSize()) {
            std::wstring m = n == 1 ? L"Restock is still updating an app." : L"Restock is still updating " + std::to_wstring(n) + L" apps.";
            if (AskDialog(m, L"Quitting stops the update in progress and drops the rest of the queue.", L"Quit anyway", L"Keep updating", L"", true) != 1) return;
        }
    }
    g_quitting = true;
    SaveWindowPlacement();
    ConfigSave();
    DestroyWindow(g_hwnd);
}

// ---------------------------------------------------------------- Restock's own updates
static std::wstring RestoreJson() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"page", json::Value::Str(PageId(g_page)));
    v.Set(L"search", json::Value::Str(g_search));
    v.Set(L"scroll", json::Value::Num(g_scrollTarget));
    v.Set(L"hidden", json::Value::Boolean(!IsWindowVisible(g_hwnd)));
    v.Set(L"background", json::Value::Boolean(GetForegroundWindow() != g_hwnd));
    v.Set(L"minimized", json::Value::Boolean(IsIconic(g_hwnd) != 0));
    std::wstring out;
    json::Write(v, out);
    return out;
}

static void ApplySelf(const std::wstring& exe, const std::wstring& version, bool userAsked) {
    if (jobs::QueueSize() > 0 || jobs::Scanning()) {
        g_applyWhenIdle = true;
        if (userAsked) Toast(L"Restock will restart once the current updates finish.");
        return;
    }
    g_applyWhenIdle = false;
    SaveWindowPlacement();
    ConfigSave();
    selfupdate::SaveRestoreState(RestoreJson());
    Log(L"self-update: restarting into %ls", version.c_str());
    tray::Remove();   // the new copy adds its own
    std::wstring err;
    if (selfupdate::Apply(g_hwnd, exe, version, &err)) {
        jobs::Shutdown();
        g_quitting = true;
        DestroyWindow(g_hwnd);
        return;
    }
    tray::Add(g_hwnd, TrayTip());
    DeleteFileW((DataDir() + L"\\restore.json").c_str());
    if (IStartsWith(err, L"Restock " + version + L" didn't start")) {
        SkipVersion(version);
        ConfigSave();
        g_self = SelfUpdateState{};
        g_self.kind = SelfUpdateState::Error;
        g_self.message = err;
    }
    Toast(err, true);
    Log(L"self-update: %ls", err.c_str());
}

static void MaybeAutoApplySelf() {
    if (g_self.kind != SelfUpdateState::Ready || jobs::QueueSize() > 0) return;
    if (!g_cfg.selfAuto && !g_applyWhenIdle) return;
    if (!g_whatsNew.empty()) return;
    LASTINPUTINFO li{sizeof(li)};
    bool idle = GetLastInputInfo(&li) && GetTickCount() - li.dwTime > 2 * 60 * 1000;
    bool away = !IsWindowVisible(g_hwnd) || IsIconic(g_hwnd) || idle;
    if (g_applyWhenIdle || away) ApplySelf(g_self.staged, g_self.version, false);
}

static void CheckSelf(bool force) {
    g_selfCheckedTick = GetTickCount();
    selfupdate::Check(g_hwnd, force);
}

// ---------------------------------------------------------------- schedule
static void RestartPc() {
    if (AskDialog(L"Restart your PC now?", L"Save your work first. Windows will restart in a few seconds.", L"Restart", L"Not now") != 1) return;
    HANDLE tok;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{};
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    if (!ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION | SHTDN_REASON_FLAG_PLANNED))
        Toast(L"Windows didn't restart. Restart from the Start menu.", true);
}

static void ScheduleTick() {
    long long now = (long long)time(nullptr);
    if (jobs::Scanning() || jobs::QueueSize()) return;
    // the overnight run: 2 to 5 AM, when nobody has touched the PC for 10 minutes
    if (g_cfg.nightly) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        LASTINPUTINFO li{sizeof(li)};
        bool idle = GetLastInputInfo(&li) && GetTickCount() - li.dwTime > 10 * 60 * 1000;
        if (st.wHour >= 2 && st.wHour < 5 && idle && now - g_cfg.lastNightly > 20 * 3600) {
            g_cfg.lastNightly = now;
            ConfigSave();
            Log(L"overnight: checking");
            jobs::Scan(g_cfg.includeUnknown, true, true);
            return;
        }
    }
    if (g_cfg.schedule == L"off") return;
    long long every = g_cfg.schedule == L"weekly" ? 7 * 86400 : 86400;
    if (now - g_cfg.lastCheck >= every) {
        Log(L"schedule: checking (%ls)", g_cfg.schedule.c_str());
        jobs::Scan(g_cfg.includeUnknown, true);
        RefreshTray();
    }
}

static void SetSetting(int setting, int option) {
    switch (setting) {
        case SSchedule: g_cfg.schedule = option == 0 ? L"off" : option == 2 ? L"weekly" : L"daily"; break;
        case SNotify: g_cfg.notify = option == 0; break;
        case SNightly: g_cfg.nightly = option == 0; break;
        case STray:
            g_cfg.tray = option == 0;
            if (g_cfg.tray) tray::Add(g_hwnd, TrayTip());
            else {
                // without the tray icon there's nothing to start with Windows, and no schedule when the window is closed
                tray::Remove();
                if (g_cfg.startup) { g_cfg.startup = false; tray::SetStartWithWindows(false); }
                Toast(L"Closing the window now quits Restock. The schedule runs only while it's open.");
            }
            break;
        case SStartup:
            g_cfg.startup = option == 0;
            tray::SetStartWithWindows(g_cfg.startup);
            if (g_cfg.startup && !g_cfg.tray) { g_cfg.tray = true; tray::Add(g_hwnd, TrayTip()); }
            break;
        case SSelfUpdating: g_cfg.includeSelfUpdating = option == 0; break;
        case SUnknown:
            if (g_cfg.includeUnknown != (option == 0)) { g_cfg.includeUnknown = option == 0; RescanSoon(); }
            break;
        case SReopen: g_cfg.reopen = option == 0; break;
        case SSelfChannel: g_cfg.selfBeta = option == 1; ConfigSave(); CheckSelf(true); break;
        case SSelfAuto: g_cfg.selfAuto = option == 0; break;
    }
    ConfigSave();
    RefreshTray();
}

static void StartWingetSetup() {
    g_wingetSetupTried = true;
    g_wingetBusy = true;
    g_wingetStatus = L"Setting up winget…";
    jobs::SetupWinget();
    StartAnim();
}

// ---------------------------------------------------------------- clicks and keys
static void Click(const HotRect& h) {
    if (!h.enabled) return;
    switch (h.kind) {
        case Hit::Nav: if (h.arg >= 0 && h.arg < (int)g_navIds.size()) ShowNav(g_navIds[h.arg]); break;
        case Hit::Search: g_searchFocus = true; g_caretOn = true; SetTimer(g_hwnd, TIMER_CARET, 530, nullptr); break;
        case Hit::SearchClear: g_search.clear(); g_focus = -1; g_scrollTarget = 0; break;
        case Hit::Refresh: RescanSoon(); break;
        case Hit::UpdateAll: UpdateAll(); break;
        case Hit::Row: {
            for (size_t k = 0; k < g_visible.size(); ++k) if (g_visible[k] == h.index) g_focus = (int)k;
            break;
        }
        case Hit::RowAction: DoPrimary(h.index); break;
        case Hit::RowMore: {
            POINT pt;
            GetCursorPos(&pt);
            RowMenu(h.index, &pt);
            break;
        }
        case Hit::BannerStop: jobs::CancelWaiting(); Toast(L"Restock will stop after the current update."); break;
        case Hit::BannerRestart: RestartPc(); break;
        case Hit::BannerWinget: case Hit::SetWingetRepair: StartWingetSetup(); break;
        case Hit::OpenHangar: OpenHangar(); break;
        case Hit::HistClear:
            if (AskDialog(L"Clear the update history?", L"This only clears Restock's list. Your apps aren't touched.", L"Clear", L"Cancel") == 1) {
                history::Clear();
                g_hist.clear();
            }
            break;
        case Hit::Seg: SetSetting(h.index, h.arg); break;
        case Hit::SetManage: Navigate(Page::Hidden); break;
        case Hit::ThemeBrowse: Navigate(Page::Themes); break;
        case Hit::ThemePick: {
            auto all = AllThemes();
            if (h.arg >= 0 && h.arg < (int)all.size()) { g_cfg.theme = all[h.arg].id; ConfigSave(); ApplyTheme(); }
            break;
        }
        case Hit::AccentPick:
            if (h.arg == 0) g_cfg.accent = L"theme";
            else if (h.arg == 1) g_cfg.accent = L"windows";
            else if (h.arg >= 2 && h.arg < 2 + kAccentSwatchCount) g_cfg.accent = HexColor(kAccentSwatches[h.arg - 2]);
            else if (!PickCustomAccent()) break;
            ConfigSave();
            ApplyTheme();
            break;
        case Hit::ThemeExport: ExportTheme(); break;
        case Hit::ThemeFolder:
            EnsureDir(ThemesDir());
            ShellExecuteW(g_hwnd, L"open", ThemesDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Hit::ThemeReload:
            g_customThemes = LoadCustomThemes();
            ReadAnimationSetting();
            ApplyTheme();
            Toast(std::to_wstring(g_customThemes.size()) + (g_customThemes.size() == 1 ? L" custom theme loaded." : L" custom themes loaded."));
            break;
        case Hit::SetCheckNow: RescanSoon(); break;
        case Hit::SetOpenData: ShellExecuteW(g_hwnd, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Hit::SetOpenLog: ShellExecuteW(g_hwnd, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Hit::SetRepo: OpenUrl(L"https://github.com/larosa023/Restock"); break;
        case Hit::ToastClose: g_toastAt = 0; break;
        case Hit::SelfRestart: if (g_self.kind == SelfUpdateState::Ready) ApplySelf(g_self.staged, g_self.version, true); break;
        case Hit::SelfCheck: CheckSelf(true); break;
        case Hit::SelfRollback: {
            auto prev = g_prevVersions;
            if (h.arg < 0 || h.arg >= (int)prev.size()) break;
            if (AskDialog(L"Go back to Restock " + prev[h.arg].version + L"?",
                          std::wstring(L"Restock restarts and won't install ") + RESTOCK_VERSION + L" again automatically. Your settings and history aren't touched.",
                          L"Roll back", L"Cancel") != 1) break;
            SkipVersion(RESTOCK_VERSION);
            ApplySelf(prev[h.arg].exe, prev[h.arg].version, true);
            break;
        }
        case Hit::WhatsNewClose: g_whatsNew.clear(); break;
        default: break;
    }
    if (h.kind != Hit::Search && h.kind != Hit::SearchClear) g_searchFocus = false;
    StartAnim();
}

static void Scroll(float delta) {
    g_scrollTarget -= delta;
    ClampScroll();
    StartAnim();
}

static void FocusSearch() {
    if (!IsListPage(g_page)) Navigate(g_lastList);
    g_searchFocus = true;
    g_caretOn = true;
    SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    Invalidate();
}

static void OnKey(WPARAM vk) {
    if (!g_whatsNew.empty()) {
        if (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_SPACE) { g_whatsNew.clear(); Invalidate(); }
        return;
    }
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool alt = GetKeyState(VK_MENU) < 0;
    bool shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl && (vk == 'K' || vk == 'F')) { FocusSearch(); return; }
    if (ctrl && vk == 'U') { UpdateAll(); return; }
    if (vk == VK_F5 || (ctrl && vk == 'R')) { RescanSoon(); Toast(L"Checking for updates…"); return; }
    if (ctrl && vk == VK_OEM_COMMA) { Navigate(Page::Settings); return; }
    if (ctrl && vk == 'H') { Navigate(Page::History); return; }
    if (ctrl && vk >= '1' && vk <= '5') {
        static const Page pages[] = {Page::Updates, Page::All, Page::SelfUpdating, Page::Hidden, Page::History};
        Navigate(pages[vk - '1']);
        return;
    }
    if (ctrl && vk == 'W') { if (g_cfg.tray && tray::Shown()) HideToTray(); else Quit(false); return; }
    if ((alt && vk == VK_LEFT) || vk == VK_BROWSER_BACK) { if (!IsListPage(g_page)) Navigate(g_lastList); return; }

    if (g_searchFocus) {
        if (ctrl && vk == 'A') { g_searchAll = !g_search.empty(); Invalidate(); return; }
        if (vk != VK_CONTROL && vk != VK_SHIFT) {
            bool wasAll = g_searchAll;
            g_searchAll = false;
            if (wasAll && vk == VK_DELETE) { g_search.clear(); g_scrollTarget = 0; Invalidate(); return; }
            if (wasAll && (vk == VK_BACK || (vk >= 0x30 && vk <= 0x5A) || vk == VK_SPACE || (vk >= VK_OEM_1 && vk <= VK_OEM_8))) {
                g_search.clear();
                if (vk == VK_BACK) { g_focus = -1; g_scrollTarget = 0; Invalidate(); g_suppressBack = true; return; }
            }
        }
        if (vk == VK_ESCAPE) {
            if (!g_search.empty()) { g_search.clear(); g_scrollTarget = 0; }
            else g_searchFocus = false;
            Invalidate();
            return;
        }
        if (vk == VK_DOWN || vk == VK_RETURN) {
            g_searchFocus = false;
            g_keyboardMode = true;
            if (g_page != Page::History) { ComputeVisible(); if (!g_visible.empty()) g_focus = 0; }
            Invalidate();
            return;
        }
        return;   // characters arrive in WM_CHAR
    }

    if (!IsListPage(g_page)) {
        if (vk == VK_ESCAPE || vk == VK_BACK) Navigate(g_lastList);
        else if (vk == VK_DOWN) Scroll(-60);
        else if (vk == VK_UP) Scroll(60);
        else if (vk == VK_NEXT) Scroll(-(g_viewH - 80));
        else if (vk == VK_PRIOR) Scroll(g_viewH - 80);
        return;
    }
    if (vk == VK_ESCAPE) {
        if (!g_search.empty()) { g_search.clear(); g_scrollTarget = 0; Invalidate(); }
        return;
    }
    if (g_page == Page::History) {
        if (vk == VK_DOWN) Scroll(-60);
        else if (vk == VK_UP) Scroll(60);
        else if (vk == VK_NEXT) Scroll(-(g_viewH - 80));
        else if (vk == VK_PRIOR) Scroll(g_viewH - 80);
        else if (vk == VK_HOME) { g_scrollTarget = 0; StartAnim(); }
        else if (vk == VK_END) { g_scrollTarget = g_contentH; ClampScroll(); StartAnim(); }
        return;
    }
    ComputeVisible();
    int n = (int)g_visible.size();
    if (!n) return;
    int f = g_focus;
    int page = std::max(1, (int)(g_viewH / (kRowH + kRowGap)) - 1);
    switch (vk) {
        case VK_DOWN: f = f < 0 ? 0 : std::min(n - 1, f + 1); break;
        case VK_UP: f = f < 0 ? 0 : std::max(0, f - 1); break;
        case VK_NEXT: f = f < 0 ? 0 : std::min(n - 1, f + page); break;
        case VK_PRIOR: f = f < 0 ? 0 : std::max(0, f - page); break;
        case VK_HOME: f = 0; break;
        case VK_END: f = n - 1; break;
        case VK_RETURN: case VK_SPACE: if (f >= 0 && f < n) DoPrimary(g_visible[f]); return;
        case VK_APPS: case VK_F10:
            if ((vk == VK_APPS || shift) && f >= 0 && f < n) {
                float top = RowTop(f) - g_scroll;
                POINT pt{(LONG)((g_W - 60) * g_scale), (LONG)((top + kRowH) * g_scale)};
                ClientToScreen(g_hwnd, &pt);
                RowMenu(g_visible[f], &pt);
            }
            return;
        default: return;
    }
    g_focus = f;
    g_keyboardMode = true;
    EnsureFocusVisible();
    Invalidate();
}

static void OnChar(wchar_t c) {
    if (c == 8 && g_suppressBack) { g_suppressBack = false; return; }
    if (c < 32 || c == 127) {
        if (c == 8 && g_searchFocus && !g_search.empty()) {
            if (GetKeyState(VK_CONTROL) < 0) {
                size_t p = g_search.find_last_of(L' ', g_search.size() - 1);
                g_search = p == std::wstring::npos ? L"" : g_search.substr(0, p);
            } else g_search.pop_back();
            g_focus = -1;
            g_scrollTarget = 0;
            Invalidate();
        } else if (c == 22 && g_searchFocus) {
            if (OpenClipboard(g_hwnd)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = (const wchar_t*)GlobalLock(h)) { g_search += Trim(p).substr(0, 64); GlobalUnlock(h); }
                }
                CloseClipboard();
                Invalidate();
            }
        }
        return;
    }
    if (!IsListPage(g_page)) return;
    if (!g_searchFocus) {
        if (c == L' ') return;   // Space updates the focused app
        g_searchFocus = true;
        SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    }
    if (g_search.size() < 64) g_search += c;
    g_caretOn = true;
    g_focus = -1;
    g_scrollTarget = 0;
    Invalidate();
}

// ---------------------------------------------------------------- messages from workers
static void PollProcesses() {
    std::vector<std::wstring> keys, dirs;
    for (auto& s : g_pkgs) {
        if (s.p.runDir.empty() || s.suite) continue;
        // only what can be updated needs to know whether it's open
        if (!HasUpdate(s) && !s.queued) continue;
        keys.push_back(s.p.key);
        dirs.push_back(s.p.runDir);
    }
    if (keys.empty()) return;
    jobs::ScanProcesses(keys, dirs);
}

static void OnScan(ScanResult* r) {
    bool first = !g_haveScan;
    g_haveScan = true;
    ApplyInventory(r->inv);
    g_cfg.lastCheck = r->inv.scannedAt;
    ConfigSave();
    if (r->inv.wingetMissing && !g_wingetSetupTried && !g_wingetBusy) {
        Log(L"winget is missing; installing it");
        StartWingetSetup();
    }
    if (r->overnight) QueueOvernight();
    else if (r->background || !WindowShown()) MaybeNotify();
    if (first) Log(L"first scan: %d update(s) ready", UpdateCount());
    PollProcesses();
    RefreshTray();
    if (g_applyWhenIdle) SetTimer(g_hwnd, TIMER_SELF, 1500, nullptr);
}

static void OnProcs(ProcResult* r) {
    bool changed = false;
    for (size_t i = 0; i < r->keys.size(); ++i)
        if (PkgState* s = FindPkg(r->keys[i])) {
            std::vector<DWORD> p = r->pids[i];
            std::sort(p.begin(), p.end());
            if (p != s->pids) { s->pids = std::move(p); changed = true; }
        }
    if (changed) Invalidate();
}

static void OnProgress(UpdateProgress* p) {
    PkgState* s = FindPkg(p->key);
    if (!s) return;
    s->queued = true;
    s->qstate = p->state;
    if (p->percent >= 0 && s->percent < 0) s->shownPercent = (float)p->percent;
    s->percent = p->percent;
    if (!(s->opText == L"Stopping…" && p->state != QueueState::Waiting)) s->opText = p->text;
    RefreshTray();
    StartAnim();
}

static void OnDone(UpdateDone* d) {
    PkgState* s = FindPkg(d->key);
    ++g_queueDone;
    bool good = d->outcome == wg::Outcome::Updated || d->outcome == wg::Outcome::Restart || d->outcome == wg::Outcome::UpToDate;
    if (s) {
        s->queued = false;
        s->percent = -1;
        s->resultText = d->message;
        if (good) {
            s->result = d->outcome == wg::Outcome::Restart ? PkgState::Restart : PkgState::Ok;
            if (!s->p.available.empty()) s->p.version = s->p.available;
            s->p.available.clear();
            if (d->outcome == wg::Outcome::Updated) s->resultText = L"Updated to " + (d->to.empty() ? s->p.version : d->to);
        } else {
            s->result = d->skipped || d->outcome == wg::Outcome::Cancelled ? PkgState::Skipped : PkgState::Failed;
        }
    }
    if (d->outcome == wg::Outcome::Restart && std::find(g_restartNeeded.begin(), g_restartNeeded.end(), d->name) == g_restartNeeded.end())
        g_restartNeeded.push_back(d->name);
    if (d->overnight && good) ++g_overnightCount;
    if (!good && !d->skipped && !d->overnight && d->outcome != wg::Outcome::Cancelled) Toast(d->name + L": " + d->message, true);
    if (g_page == Page::History) { g_hist = history::Load(); std::reverse(g_hist.begin(), g_hist.end()); }
    RefreshTray();
    StartAnim();
}

static void OnQueueIdle(int ran) {
    int ok = 0, failed = 0;
    std::vector<std::wstring> failedNames;
    for (auto& s : g_pkgs) {
        if (s.result == PkgState::Ok || s.result == PkgState::Restart) ++ok;
        if (s.result == PkgState::Failed) { ++failed; failedNames.push_back(s.p.name); }
    }
    if (g_overnightRun) {
        g_overnightRun = false;
        if (g_overnightCount > 0)
            tray::Notify(L"Restock updated " + std::to_wstring(g_overnightCount) + (g_overnightCount == 1 ? L" app overnight" : L" apps overnight"),
                         L"See History for the details.");
    } else if (ran > 1) {
        Toast(failed ? std::to_wstring(ok) + L" updated, " + std::to_wstring(failed) + L" failed. See each app for why."
                     : std::to_wstring(ok) + L" apps updated.", failed > 0);
        if (!WindowShown()) tray::Notify(L"Updates finished", failed ? std::to_wstring(ok) + L" updated, " + JoinNames(failedNames) + L" didn't." : std::to_wstring(ok) + L" apps updated.");
    }
    g_queueTotal = g_queueDone = 0;
    // see what changed on disk (versions, and what's left to update)
    RescanSoon(true);
    if (g_applyWhenIdle) SetTimer(g_hwnd, TIMER_SELF, 1500, nullptr);
    RefreshTray();
}

// ---------------------------------------------------------------- window
static void SaveWindowPlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    g_cfg.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    RECT r = wp.rcNormalPosition;
    g_cfg.windowX = r.left;
    g_cfg.windowY = r.top;
    g_cfg.windowW = r.right - r.left;
    g_cfg.windowH = r.bottom - r.top;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == tray::TaskbarCreatedMessage() && msg) {
        // Explorer restarted: put the icon back
        if (g_cfg.tray) tray::Add(hwnd, TrayTip());
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            g_scale = GetDpiForWindow(hwnd) / 96.0f;
            g_customThemes = LoadCustomThemes();
            ApplyTheme();
            jobs::Init(hwnd);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (IsWindowVisible(hwnd)) Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            if (wp == SIZE_MINIMIZED) {
                KillTimer(hwnd, TIMER_PROCS);
                gfx::ClearTextCaches();
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            } else if (IsWindowVisible(hwnd)) {
                SetTimer(hwnd, TIMER_PROCS, 1500, nullptr);
            }
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            float s = GetDpiForWindow(hwnd) / 96.0f;
            mm->ptMinTrackSize.x = (LONG)(840 * s);
            mm->ptMinTrackSize.y = (LONG)(560 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_scale = HIWORD(wp) / 96.0f;
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_rt) g_rt->SetDpi((float)HIWORD(wp), (float)HIWORD(wp));
            Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
            ReadAnimationSetting();
            if (lp && (wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0)) {
                InvalidateSystemColors();
                ApplyTheme();
                Invalidate();
            }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            InvalidateSystemColors();
            ApplyTheme();
            return 0;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            if (g_dragThumb) { DragThumbTo(y); return 0; }
            if (!g_tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                g_tracking = true;
            }
            HotRect h = HitAt(x, y);
            if (h.kind != g_hover.kind || h.index != g_hover.index || h.arg != g_hover.arg) {
                g_hover = h;
                StartAnim();
            }
            if (g_keyboardMode) { g_keyboardMode = false; Invalidate(); }
            bool hand = h.kind != Hit::None && h.enabled && h.kind != Hit::Search && h.kind != Hit::ScrollThumb && h.kind != Hit::ScrollTrack && h.kind != Hit::Row;
            SetCursor(LoadCursorW(nullptr, h.kind == Hit::Search ? IDC_IBEAM : hand ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;
            break;
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hover = {{0, 0, 0, 0}, Hit::None, -1, 0, false};
            StartAnim();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            g_pressed = HitAt(x, y);
            if (g_pressed.kind == Hit::None && g_searchFocus) g_searchFocus = false;
            if (g_pressed.kind == Hit::ScrollThumb) {
                g_dragThumb = true;
                g_dragGrab = y - g_thumbTop;
            } else if (g_pressed.kind == Hit::ScrollTrack) {
                Scroll(y < g_thumbTop ? g_viewH - 60 : -(g_viewH - 60));
            }
            SetCapture(hwnd);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (g_dragThumb) { g_dragThumb = false; g_pressed = {{0, 0, 0, 0}, Hit::None, -1, 0, false}; Invalidate(); return 0; }
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            HotRect up = HitAt(x, y);
            HotRect down = g_pressed;
            g_pressed = {{0, 0, 0, 0}, Hit::None, -1, 0, false};
            if (up.kind == down.kind && up.index == down.index && up.arg == down.arg && up.kind != Hit::None) Click(up);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            HotRect h = HitAt(x, y);
            if (h.kind == Hit::Row) DoPrimary(h.index);
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            HotRect h = HitAt(x, y);
            if (h.kind == Hit::Row || h.kind == Hit::RowAction || h.kind == Hit::RowMore) RowMenu(h.index, nullptr);
            return 0;
        }
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1 && !IsListPage(g_page)) Navigate(g_lastList);
            return TRUE;
        case WM_MOUSEWHEEL:
            Scroll(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 96.0f);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && wp != VK_LEFT && wp != VK_F10) break;
            OnKey(wp);
            return 0;
        case WM_CHAR:
            OnChar((wchar_t)wp);
            return 0;
        case WM_TIMER:
            switch (wp) {
                case TIMER_ANIM:
                    if (!Animate() || !WindowShown()) { KillTimer(hwnd, TIMER_ANIM); g_animTimer = false; }
                    Invalidate();
                    break;
                case TIMER_CARET:
                    if (!g_searchFocus || GetFocus() != hwnd) { KillTimer(hwnd, TIMER_CARET); g_caretOn = false; }
                    else g_caretOn = !g_caretOn;
                    Invalidate();
                    break;
                case TIMER_PROCS:
                    SetTimer(hwnd, TIMER_PROCS, GetForegroundWindow() == hwnd ? 3000 : 8000, nullptr);
                    if (WindowShown()) PollProcesses();
                    else KillTimer(hwnd, TIMER_PROCS);
                    break;
                case TIMER_SCHEDULE:
                    SetTimer(hwnd, TIMER_SCHEDULE, 5 * 60 * 1000, nullptr);
                    ScheduleTick();
                    break;
                case TIMER_SELF:
                    SetTimer(hwnd, TIMER_SELF, 20 * 1000, nullptr);
                    // on launch, then every 3 hours
                    if (!g_selfCheckedTick || GetTickCount() - g_selfCheckedTick > 3u * 3600 * 1000) CheckSelf(false);
                    MaybeAutoApplySelf();
                    break;
                case TIMER_HEALTHY:
                    KillTimer(hwnd, TIMER_HEALTHY);
                    selfupdate::SignalHealthy(g_after);
                    selfupdate::FinishAfterUpdate(g_after);
                    break;
                case TIMER_MEMNOTE: {
                    KillTimer(hwnd, TIMER_MEMNOTE);
                    PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
                    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
                    Log(L"perf: after 1 minute: working set %.1f MB, private %.1f MB, peak %.1f MB",
                        pmc.WorkingSetSize / 1048576.0, pmc.PagefileUsage / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0);
                    break;
                }
                case TIMER_TOAST:
                    KillTimer(hwnd, TIMER_TOAST);
                    g_toastAt = 0;
                    StartAnim();
                    break;
            }
            return 0;
        case WM_ACTIVATEAPP:
            if (wp && WindowShown()) { PollProcesses(); SetTimer(hwnd, TIMER_PROCS, 3000, nullptr); }
            return 0;
        case WM_KILLFOCUS:
            g_searchFocus = false;
            Invalidate();
            return 0;
        case WM_RESTOCK_SCAN: { std::unique_ptr<ScanResult> r((ScanResult*)lp); OnScan(r.get()); Invalidate(); return 0; }
        case WM_RESTOCK_ICON: {
            std::unique_ptr<IconResult> r((IconResult*)lp);
            if (PkgState* s = FindPkg(r->key)) {
                SafeRelease(s->icon);
                s->iconPixels = std::move(r->pixels);
                s->iconW = r->w;
                s->iconH = r->h;
                Invalidate();
            }
            return 0;
        }
        case WM_RESTOCK_PROCS: { std::unique_ptr<ProcResult> r((ProcResult*)lp); OnProcs(r.get()); return 0; }
        case WM_RESTOCK_PROGRESS: { std::unique_ptr<UpdateProgress> r((UpdateProgress*)lp); OnProgress(r.get()); return 0; }
        case WM_RESTOCK_DONE: { std::unique_ptr<UpdateDone> r((UpdateDone*)lp); OnDone(r.get()); return 0; }
        case WM_RESTOCK_QUEUE_IDLE: OnQueueIdle((int)wp); Invalidate(); return 0;
        case WM_RESTOCK_SUITE: { std::unique_ptr<SuiteResult> r((SuiteResult*)lp); ApplySuite(r->text); RefreshTray(); Invalidate(); return 0; }
        case WM_RESTOCK_WINGET: {
            std::unique_ptr<WingetSetupResult> r((WingetSetupResult*)lp);
            g_wingetStatus = r->message;
            if (r->done) {
                g_wingetBusy = false;
                if (r->ok) { g_wingetStatus.clear(); Toast(L"winget is ready. Checking for updates…"); RescanSoon(); }
                else Toast(r->message, true);
            }
            StartAnim();
            Invalidate();
            return 0;
        }
        case WM_RESTOCK_ASK: {
            std::unique_ptr<AskRequest> a((AskRequest*)lp);
            if (!WindowShown()) ShowFromTray();
            int ans = AskDialog(a->heading, a->text, a->yes, a->no);
            a->reply->answer = ans == 1 ? 1 : 0;
            SetEvent(a->reply->done);
            return 0;
        }
        case WM_RESTOCK_SELF: {
            std::unique_ptr<SelfUpdateState> r((SelfUpdateState*)lp);
            bool wasReady = g_self.kind == SelfUpdateState::Ready;
            if (!(wasReady && (r->kind == SelfUpdateState::Error || r->kind == SelfUpdateState::Checking))) g_self = *r;
            if (r->kind == SelfUpdateState::Ready && !wasReady) {
                if (!g_cfg.selfAuto) Toast(L"Restock " + r->version + L" is ready. Restart Restock to update.");
                MaybeAutoApplySelf();
            }
            Invalidate();
            return 0;
        }
        case WM_RESTOCK_TRAY: {
            UINT ev = LOWORD(lp);
            if (ev == NIN_SELECT || ev == NIN_KEYSELECT || ev == WM_LBUTTONUP) ShowFromTray();
            else if (ev == NIN_BALLOONUSERCLICK) ShowFromTray(UpdateCount() || jobs::QueueSize() ? Page::Updates : Page::History);
            else if (ev == WM_CONTEXTMENU || ev == WM_RBUTTONUP) {
                UINT cmd = tray::Menu(hwnd, UpdateCount(), jobs::Scanning() || jobs::QueueSize() > 0);
                switch (cmd) {
                    case tray::CmdOpen: ShowFromTray(); break;
                    case tray::CmdCheck: RescanSoon(!WindowShown()); RefreshTray(); break;
                    case tray::CmdUpdateAll: ShowFromTray(Page::Updates); UpdateAll(); break;
                    case tray::CmdHistory: ShowFromTray(Page::History); break;
                    case tray::CmdSettings: ShowFromTray(Page::Settings); break;
                    case tray::CmdQuit: Quit(false); break;
                }
            }
            return 0;
        }
        case WM_RESTOCK_ACTIVATE:
            ShowFromTray();
            return 0;
        case WM_RESTOCK_QUIT:
            // Restock.exe --quit (Hangar runs this before it updates Restock)
            Quit(true);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) { SaveWindowPlacement(); ConfigSave(); }
            return 0;
        case WM_CLOSE:
            if (!g_quitting && g_cfg.tray && tray::Shown()) { HideToTray(); return 0; }
            Quit(false);
            return 0;
        case WM_DESTROY:
            tray::Remove();
            jobs::Shutdown();
            DiscardDevice();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND UiCreate(HINSTANCE inst, int show, const selfupdate::AfterUpdate& after, bool startHidden) {
    g_after = after;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_startQpc);
    wchar_t pv[8];
    g_perf = GetEnvironmentVariableW(L"RESTOCK_PERF", pv, 8) > 0;
    if (!gfx::Init()) return nullptr;
    ReadAnimationSetting();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"Restock.larosa023.Main";
    RegisterClassExW(&wc);

    // the suite list (cached, else the copy built into the exe) and the last scan, so the window
    // opens instantly with real results; a fresh scan follows in the background
    std::wstring suiteText;
    if (!ReadTextFile(CacheDir() + L"\\hangar-manifest.json", suiteText) || !g_suite.Load(suiteText)) {
        if (LoadResourceText(100, suiteText)) g_suite.Load(suiteText);
    }
    Inventory cached;
    if (LoadInventoryCache(cached)) ApplyInventory(cached);
    g_page = NavPage(g_cfg.page);
    if (!IsListPage(g_page)) g_page = Page::Updates;
    g_lastList = g_page;

    UINT dpi = GetDpiForSystem();
    float s = dpi / 96.0f;
    int w = g_cfg.windowW > 0 ? g_cfg.windowW : (int)(1180 * s);
    int h = g_cfg.windowH > 0 ? g_cfg.windowH : (int)(780 * s);
    int x = g_cfg.windowX, y = g_cfg.windowY;
    if (x != CW_USEDEFAULT) {
        RECT r{x, y, x + w, y + h};
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Restock", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;
    PruneSkippedVersions(RESTOCK_VERSION);
    if (after.active) {
        // a Restock update just handed over: open where the previous copy was
        std::wstring text = selfupdate::TakeRestoreState();
        json::Value v;
        if (!text.empty() && json::Parser(text).Parse(v) && v.type == json::Value::Object) {
            Page p = NavPage(v.GetStr(L"page", L"updates"));
            g_page = p;
            if (IsListPage(p)) g_lastList = p;
            if (p == Page::History) { g_hist = history::Load(); std::reverse(g_hist.begin(), g_hist.end()); }
            g_search = v.GetStr(L"search");
            g_scroll = g_scrollTarget = (float)v.GetNum(L"scroll", 0);
            if (v.GetBool(L"hidden", false)) startHidden = true;
            else if (v.GetBool(L"minimized", false)) show = SW_SHOWMINNOACTIVE;
            else if (v.GetBool(L"background", false)) show = SW_SHOWNOACTIVATE;
        }
        if (CompareVersions(after.fromVersion, RESTOCK_VERSION) < 0) {
            std::wstring notes;
            LoadResourceText(101, notes);
            g_whatsNew = CleanNotes(notes);
            if (g_whatsNew.empty()) g_whatsNew = L"See the release notes on GitHub.";
            g_whatsNewVersion = RESTOCK_VERSION;
        } else {
            Toast(std::wstring(L"Rolled back to Restock ") + RESTOCK_VERSION + L".");
        }
        Log(L"started after an update from %ls", after.fromVersion.c_str());
    }
    // the tray icon runs the schedule
    if (g_cfg.startup && !g_cfg.tray) g_cfg.startup = false;
    tray::SetStartWithWindows(g_cfg.startup);   // keeps the path right if Restock moved
    if (g_cfg.tray) tray::Add(hwnd, TrayTip());
    if (startHidden && !tray::Shown()) startHidden = false;   // never invisible with no way back
    if (!startHidden) {
        bool maximize = g_cfg.maximized && (show == SW_SHOWNORMAL || show == SW_SHOWNOACTIVATE);
        ShowWindow(hwnd, maximize ? SW_SHOWMAXIMIZED : show);
        UpdateWindow(hwnd);
        SetTimer(hwnd, TIMER_PROCS, 1500, nullptr);
    }
    if (after.active) SetTimer(hwnd, TIMER_HEALTHY, 400, nullptr);

    // then refresh in the background: when you opened it, always; from the tray, only when it's due
    jobs::FetchSuite();
    if (!startHidden) jobs::Scan(g_cfg.includeUnknown, false);
    // from the tray, the first scheduled look comes soon after sign-in
    SetTimer(hwnd, TIMER_SCHEDULE, startHidden ? 45 * 1000 : 5 * 60 * 1000, nullptr);
    SetTimer(hwnd, TIMER_MEMNOTE, 60 * 1000, nullptr);
    SetTimer(hwnd, TIMER_SELF, 20 * 1000, nullptr);
    return hwnd;
}
