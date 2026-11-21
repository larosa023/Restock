// Restock - background work
#include "jobs.h"
#include "winget.h"
#include "history.h"
#include "net.h"
#include "util.h"
#include <shellapi.h>
#include <objbase.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <chrono>
#include <set>
#include <map>
#include <ctime>
#include <algorithm>

namespace jobs {

static HWND s_hwnd = nullptr;
static std::atomic<bool> s_shutdown{false};

template <class T> static void Post(UINT msg, T* obj, WPARAM wp = 0) {
    if (s_shutdown || !s_hwnd || !PostMessageW(s_hwnd, msg, wp, (LPARAM)obj)) delete obj;
}

// Short jobs (icons, process polls, the manifest) share a few worker threads that exit when idle.
class Pool {
public:
    void Submit(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> l(m_);
            q_.push_back(std::move(f));
            if ((int)q_.size() > idle_ && threads_ < kMax) { ++threads_; std::thread([this] { Worker(); }).detach(); }
        }
        cv_.notify_one();
    }
private:
    static constexpr int kMax = 3;
    void Worker() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::unique_lock<std::mutex> l(m_);
        for (;;) {
            ++idle_;
            bool got = cv_.wait_for(l, std::chrono::seconds(60), [&] { return !q_.empty(); });
            --idle_;
            if (!got) break;
            auto f = std::move(q_.front());
            q_.pop_front();
            l.unlock();
            f();
            l.lock();
        }
        --threads_;
        l.unlock();
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    int threads_ = 0;
    int idle_ = 0;
};
static Pool& s_pool = *new Pool;   // never destroyed: workers may still be parked when the process exits

template <class F> static void Run(F&& f) { s_pool.Submit(std::function<void()>(std::forward<F>(f))); }

template <class F> static void RunDedicated(F&& f) {
    std::thread([f = std::forward<F>(f)]() mutable {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        f();
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
}

void Init(HWND notify) { s_hwnd = notify; }

// ---------------------------------------------------------------- scanning
// One scan at a time (winget locks its database); a request during a scan runs once more after it.
static std::mutex s_scanLock;
static bool s_scanning = false, s_scanAgain = false;
static bool s_againUnknown = true, s_againBackground = true, s_againOvernight = false;

bool Scanning() { std::lock_guard<std::mutex> g(s_scanLock); return s_scanning; }

void Scan(bool includeUnknown, bool background, bool overnight) {
    {
        std::lock_guard<std::mutex> g(s_scanLock);
        if (s_scanning) {
            s_againUnknown = includeUnknown;
            s_againBackground = s_scanAgain ? (s_againBackground && background) : background;
            s_againOvernight = (s_scanAgain && s_againOvernight) || overnight;
            s_scanAgain = true;
            return;
        }
        s_scanning = true;
    }
    RunDedicated([includeUnknown, background, overnight] {
        bool unknown = includeUnknown, bg = background, night = overnight;
        for (;;) {
            DWORD t0 = GetTickCount();
            auto* res = new ScanResult;
            res->inv = ScanInventory(unknown);
            res->background = bg;
            res->overnight = night;
            SaveInventoryCache(res->inv);
            Log(L"scan: %zu apps in %lu ms%ls", res->inv.pkgs.size(), GetTickCount() - t0, res->inv.wingetOk ? L"" : L" (winget unavailable)");
            std::lock_guard<std::mutex> g(s_scanLock);
            if (!s_scanAgain) {
                s_scanning = false;
                Post(WM_RESTOCK_SCAN, res);
                return;
            }
            Post(WM_RESTOCK_SCAN, res);
            s_scanAgain = false;
            unknown = s_againUnknown; bg = s_againBackground; night = s_againOvernight;
            s_againOvernight = false;
        }
    });
}

void FetchIcon(const std::wstring& key, const std::wstring& path, int index, UINT px) {
    if (path.empty()) return;
    Run([key, path, index, px] {
        auto* res = new IconResult;
        res->key = key;
        if (ExtractIconPixels(path, index, px, res->w, res->h, res->pixels)) Post(WM_RESTOCK_ICON, res);
        else delete res;
    });
}

void ScanProcesses(const std::vector<std::wstring>& keys, const std::vector<std::wstring>& dirs) {
    static std::atomic<bool> pending{false};
    if (pending.exchange(true)) return;
    Run([keys, dirs] {
        struct Clear { ~Clear() { pending = false; } } clear;
        auto* res = new ProcResult;
        res->keys = keys;
        res->pids = FindRunningIn(dirs);
        Post(WM_RESTOCK_PROCS, res);
    });
}

// Hangar's manifest: the list of suite apps Restock leaves to Hangar
void FetchSuite() {
    Run([] {
        std::wstring cache = CacheDir() + L"\\hangar-manifest.json", etagPath = CacheDir() + L"\\hangar-manifest.etag";
        std::wstring etag;
        if (FileExists(cache)) ReadTextFile(etagPath, etag);
        net::Response r;
        if (!net::Get(HANGAR_MANIFEST_URL, r, Trim(etag))) { Log(L"suite manifest: %ls", r.error.c_str()); return; }
        if (r.status == 304) return;
        if (r.status != 200) { Log(L"suite manifest: HTTP %d", r.status); return; }
        std::wstring text = Utf8ToWide(r.body);
        Suite probe;
        if (!probe.Load(text)) { Log(L"suite manifest: not valid"); return; }
        WriteTextFileAtomic(cache, text);
        WriteTextFileAtomic(etagPath, r.etag);
        auto* res = new SuiteResult;
        res->text = text;
        Post(WM_RESTOCK_SUITE, res);
    });
}

void SetupWinget() {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return;
    RunDedicated([] {
        std::wstring err;
        bool ok = winget::Install([](const std::wstring& s) {
            auto* p = new WingetSetupResult;
            p->message = s;
            Post(WM_RESTOCK_WINGET, p);
        }, &err);
        auto* res = new WingetSetupResult;
        res->done = true;
        res->ok = ok;
        res->message = ok ? L"winget is ready." : err;
        Post(WM_RESTOCK_WINGET, res);
        busy = false;
    });
}

// ---------------------------------------------------------------- the update queue
struct Item {
    UpdateRequest req;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
};

static std::mutex s_qLock;
static std::deque<Item> s_queue;
static Item s_current;
static bool s_running = false;     // the queue thread is alive
static bool s_busy = false;        // an item is being processed
static int s_ranCount = 0;

int QueueSize() {
    std::lock_guard<std::mutex> g(s_qLock);
    return (int)s_queue.size() + (s_busy ? 1 : 0);
}

std::wstring Running() {
    std::lock_guard<std::mutex> g(s_qLock);
    return s_busy ? s_current.req.key : L"";
}

bool IsQueued(const std::wstring& key) {
    std::lock_guard<std::mutex> g(s_qLock);
    if (s_busy && IEquals(s_current.req.key, key)) return true;
    for (auto& i : s_queue) if (IEquals(i.req.key, key)) return true;
    return false;
}

static void Progress(const std::wstring& key, QueueState st, int pct, const std::wstring& text) {
    auto* p = new UpdateProgress;
    p->key = key;
    p->state = st;
    p->percent = pct;
    p->text = text;
    Post(WM_RESTOCK_PROGRESS, p);
}

// Blocks the queue until you answer (or 10 minutes pass: that's a no)
static bool Ask(const std::wstring& title, const std::wstring& heading, const std::wstring& text, const std::wstring& yes, const std::wstring& no) {
    auto* a = new AskRequest;
    a->title = title;
    a->heading = heading;
    a->text = text;
    a->yes = yes;
    a->no = no;
    a->reply = std::make_shared<AskRequest::Reply>();
    a->reply->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto reply = a->reply;
    Post(WM_RESTOCK_ASK, a);
    WaitForSingleObject(reply->done, 10 * 60 * 1000);
    return reply->answer == 1;
}

struct CloseCtx { std::set<DWORD> pids; };
static BOOL CALLBACK CloseEnum(HWND hwnd, LPARAM lp) {
    auto* c = (CloseCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (c->pids.count(pid) && !GetWindow(hwnd, GW_OWNER)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

static bool WaitForExit(const std::vector<DWORD>& pids, DWORD ms) {
    DWORD start = GetTickCount();
    bool ok = true;
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!h) continue;
        DWORD left = ms - std::min<DWORD>(ms, GetTickCount() - start);
        if (WaitForSingleObject(h, left) != WAIT_OBJECT_0) ok = false;
        CloseHandle(h);
    }
    return ok;
}

static std::vector<DWORD> RunningIn(const std::wstring& dir) {
    if (dir.empty()) return {};
    auto r = FindRunningIn({dir});
    return r.empty() ? std::vector<DWORD>{} : r[0];
}

static void Finish(const Item& it, wg::Outcome outcome, const std::wstring& message, bool skipped) {
    const UpdateRequest& q = it.req;
    HistoryEntry h;
    h.time = (long long)time(nullptr);
    h.key = q.key;
    h.name = q.name;
    h.from = q.from;
    h.to = q.to;
    h.overnight = q.overnight;
    h.message = message;
    switch (outcome) {
        case wg::Outcome::Updated: h.result = L"updated"; break;
        case wg::Outcome::Restart: h.result = L"restart"; break;
        case wg::Outcome::UpToDate: h.result = L"uptodate"; break;
        case wg::Outcome::Cancelled: h.result = skipped ? L"skipped" : L"cancelled"; break;
        default: h.result = skipped ? L"skipped" : L"failed"; break;
    }
    history::Add(h);
    Log(L"update %ls %ls -> %ls: %ls (%ls)", q.key.c_str(), q.from.c_str(), q.to.c_str(), h.result.c_str(), message.c_str());
    auto* d = new UpdateDone;
    d->key = q.key;
    d->name = q.name;
    d->from = q.from;
    d->to = q.to;
    d->outcome = outcome;
    d->message = message;
    d->skipped = skipped;
    d->overnight = q.overnight;
    Post(WM_RESTOCK_DONE, d);
}

static void ProcessItem(Item& it) {
    const UpdateRequest& q = it.req;
    Progress(q.key, QueueState::Running, -1, L"Starting…");

    // 1. close it first if it's open: the polite way (WM_CLOSE), and only by force if you say so
    std::wstring reopenExe;
    bool closed = false;
    std::vector<DWORD> pids = RunningIn(q.runDir);
    if (!pids.empty()) {
        if (q.overnight) { Finish(it, wg::Outcome::Cancelled, L"Skipped because it was open", true); return; }
        if (!q.closeApproved &&
            !Ask(L"Restock", q.name + L" is open", L"Restock will close " + q.name + L", update it and open it again. Save your work in it first.",
                 L"Close and update", L"Skip it")) {
            Finish(it, wg::Outcome::Cancelled, L"Skipped because it was open", true);
            return;
        }
        reopenExe = MainExeOf(pids);
        Progress(q.key, QueueState::Closing, -1, L"Closing " + q.name + L"…");
        CloseCtx c;
        c.pids.insert(pids.begin(), pids.end());
        EnumWindows(CloseEnum, (LPARAM)&c);
        WaitForExit(pids, 10000);
        std::vector<DWORD> left = RunningIn(q.runDir);
        if (!left.empty()) {
            // usually a tray app: closing its window only hides it
            bool force = Ask(L"Restock", q.name + L" didn't close",
                             q.name + L" may still be running in the system tray. Restock can end it and continue. Anything unsaved in " + q.name + L" will be lost.",
                             L"End it and update", L"Skip it");
            if (!force) { Finish(it, wg::Outcome::Cancelled, L"Skipped because it didn't close", true); return; }
            for (DWORD pid : left)
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) { TerminateProcess(h, 0); CloseHandle(h); }
            WaitForExit(left, 8000);
            Log(L"%ls: ended it (you agreed)", q.key.c_str());
        }
        Sleep(400);
        closed = true;
    }

    // 2. update with winget
    auto progress = [&](int pct, const std::wstring& phase) {
        Progress(q.key, QueueState::Running, pct, phase.empty() ? L"Updating…" : phase);
    };
    winget::UpgradeRequest wr;
    wr.id = q.id;
    wr.source = q.source;
    wr.includeUnknown = q.includeUnknown;
    winget::UpgradeOutput out = winget::Upgrade(wr, progress, it.cancel.get());
    wg::Result res;
    if (!out.started) { res.outcome = wg::Outcome::Failed; res.message = L"winget couldn't start"; }
    else if (out.cancelled) { res.outcome = wg::Outcome::Cancelled; res.message = L"Stopped"; }
    else res = wg::Classify(out.exitCode, out.output);

    // 3. some installers need administrator rights: ask for them for this one update only
    if (res.outcome == wg::Outcome::NeedsAdmin && !*it.cancel) {
        if (q.overnight) {
            res.message = L"Needs administrator rights · update it from Restock";
        } else if (Ask(L"Restock", q.name + L" needs administrator rights", L"Its installer has to make changes for all users. Windows will ask you to allow it.",
                       L"Update as administrator", L"Not now")) {
            wr.elevated = true;
            out = winget::Upgrade(wr, progress, it.cancel.get());
            if (out.declined) { res.outcome = wg::Outcome::Cancelled; res.message = L"The administrator prompt was declined"; }
            else if (!out.started) { res.outcome = wg::Outcome::Failed; res.message = L"Couldn't start the update as administrator"; }
            else res = wg::Classify(out.exitCode, out.output);
        } else {
            res.outcome = wg::Outcome::Cancelled;
            res.message = L"Needs administrator rights";
        }
    }
    if (res.outcome == wg::Outcome::Failed || res.outcome == wg::Outcome::NeedsAdmin || res.outcome == wg::Outcome::InUse)
        Log(L"winget output for %ls (0x%08X):\r\n%ls", q.id.c_str(), out.exitCode, out.output.substr(out.output.size() > 6000 ? out.output.size() - 6000 : 0).c_str());

    // 4. put it back the way it was
    if (closed && q.reopen && res.outcome != wg::Outcome::Restart) {
        if (!reopenExe.empty() && FileExists(reopenExe)) {
            ShellExecuteW(nullptr, L"open", reopenExe.c_str(), nullptr, PathParent(reopenExe).c_str(), SW_SHOWNORMAL);
            Log(L"%ls: reopened %ls", q.key.c_str(), reopenExe.c_str());
        } else if (!reopenExe.empty()) {
            Log(L"%ls: %ls is gone after the update; not reopening", q.key.c_str(), reopenExe.c_str());
        }
    }
    Finish(it, res.outcome, res.message, false);
}

static void QueueThread() {
    for (;;) {
        Item it;
        {
            std::lock_guard<std::mutex> g(s_qLock);
            if (s_queue.empty() || s_shutdown) {
                s_running = false;
                s_busy = false;
                int n = s_ranCount;
                s_ranCount = 0;
                if (!s_shutdown && s_hwnd) PostMessageW(s_hwnd, WM_RESTOCK_QUEUE_IDLE, (WPARAM)n, 0);
                return;
            }
            it = s_queue.front();
            s_queue.pop_front();
            s_current = it;
            s_busy = true;
            ++s_ranCount;
        }
        ProcessItem(it);
        std::lock_guard<std::mutex> g(s_qLock);
        s_busy = false;
    }
}

void Enqueue(const UpdateRequest& req) {
    std::lock_guard<std::mutex> g(s_qLock);
    if (s_busy && IEquals(s_current.req.key, req.key)) return;
    for (auto& i : s_queue) if (IEquals(i.req.key, req.key)) return;
    Item it;
    it.req = req;
    s_queue.push_back(it);
    Progress(req.key, QueueState::Waiting, -1, L"Waiting…");
    if (!s_running) {
        s_running = true;
        RunDedicated([] { QueueThread(); });
    }
}

void Cancel(const std::wstring& key) {
    std::vector<Item> dropped;
    {
        std::lock_guard<std::mutex> g(s_qLock);
        if (s_busy && IEquals(s_current.req.key, key)) { *s_current.cancel = true; return; }
        for (auto i = s_queue.begin(); i != s_queue.end();) {
            if (IEquals(i->req.key, key)) { dropped.push_back(*i); i = s_queue.erase(i); }
            else ++i;
        }
    }
    for (auto& d : dropped) {
        auto* p = new UpdateDone;
        p->key = d.req.key;
        p->name = d.req.name;
        p->outcome = wg::Outcome::Cancelled;
        p->skipped = true;
        p->message = L"Removed from the queue";
        Post(WM_RESTOCK_DONE, p);
    }
}

void CancelWaiting() {
    std::deque<Item> dropped;
    {
        std::lock_guard<std::mutex> g(s_qLock);
        dropped.swap(s_queue);
    }
    for (auto& d : dropped) {
        auto* p = new UpdateDone;
        p->key = d.req.key;
        p->name = d.req.name;
        p->outcome = wg::Outcome::Cancelled;
        p->skipped = true;
        p->message = L"Removed from the queue";
        Post(WM_RESTOCK_DONE, p);
    }
}

void Shutdown() {
    s_shutdown = true;
    std::lock_guard<std::mutex> g(s_qLock);
    s_queue.clear();
    if (s_busy) *s_current.cancel = true;
}

} // namespace jobs
