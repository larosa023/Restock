// Restock - background work. Every job runs off the UI thread and posts its result to the main
// window, which owns all state. Updates run one at a time in a queue (installers don't like to
// overlap), each on the queue's own thread.
#pragma once
#include "common.h"
#include "inventory.h"
#include "wingetparse.h"
#include <memory>
#include <atomic>

struct ScanResult {
    Inventory inv;
    bool background = false;        // a scheduled check (tray), not one you asked for
    bool overnight = false;         // the overnight run: queue what's eligible once it's in
};

struct IconResult {
    std::wstring key;
    UINT w = 0, h = 0;
    std::vector<uint8_t> pixels;    // premultiplied BGRA, w*h*4
};

struct ProcResult {
    std::vector<std::wstring> keys;
    std::vector<std::vector<DWORD>> pids;
};

struct SuiteResult {
    std::wstring text;              // Hangar's manifest
};

struct WingetSetupResult {
    bool done = false;              // false: a progress message
    bool ok = false;
    std::wstring message;
};

struct UpdateRequest {
    std::wstring key, id, source, name, from, to;
    std::wstring runDir;            // where its processes run from (to close it first)
    bool closeApproved = false;     // you agreed to close it if it's open
    bool reopen = true;             // open it again afterwards if Restock closed it
    bool overnight = false;         // unattended: never asks, skips apps that are open or need admin
    bool includeUnknown = true;
};

enum class QueueState { Waiting, Closing, Running, Done };

struct UpdateProgress {
    std::wstring key;
    QueueState state = QueueState::Waiting;
    int percent = -1;               // -1: indeterminate
    std::wstring text;
};

struct UpdateDone {
    std::wstring key, name, from, to;
    wg::Outcome outcome = wg::Outcome::Failed;
    bool skipped = false;           // never started (it was open, or you said no)
    std::wstring message;
    bool overnight = false;
};

// A yes/no the queue needs from you (close an app that won't close, run as administrator…)
struct AskRequest {
    std::wstring title, heading, text, yes, no;
    struct Reply { HANDLE done = nullptr; std::atomic<int> answer{-1}; ~Reply() { if (done) CloseHandle(done); } };
    std::shared_ptr<Reply> reply;
};

namespace jobs {

void Init(HWND notify);
void Shutdown();

void Scan(bool includeUnknown, bool background, bool overnight = false);
bool Scanning();
void FetchIcon(const std::wstring& key, const std::wstring& path, int index, UINT px);
void ScanProcesses(const std::vector<std::wstring>& keys, const std::vector<std::wstring>& dirs);
void FetchSuite();
void SetupWinget();

void Enqueue(const UpdateRequest& req);
void Cancel(const std::wstring& key);       // a waiting update is dropped; the running one is stopped
void CancelWaiting();                        // drop everything that hasn't started
int QueueSize();                             // waiting + running
std::wstring Running();                      // key of the update in progress, or ""
bool IsQueued(const std::wstring& key);

} // namespace jobs
