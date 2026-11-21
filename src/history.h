// Restock - the update history (DataDir()\history.json): every update Restock ran, with the
// date, the version before and after, and how it went.
#pragma once
#include "common.h"

struct HistoryEntry {
    long long time = 0;                 // unix seconds
    std::wstring key, name, from, to;
    std::wstring result;                // updated | restart | failed | cancelled | skipped | uptodate
    std::wstring message;
    bool overnight = false;             // run by the overnight schedule
};

namespace history {
void Add(const HistoryEntry& e);        // thread-safe
std::vector<HistoryEntry> Load();       // oldest first
void Clear();
}
