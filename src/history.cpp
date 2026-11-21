// Restock - the update history
#include "history.h"
#include "util.h"
#include "json.h"
#include <mutex>

namespace history {

static std::mutex s_lock;
static const size_t kMax = 2000;

static std::wstring PathOf() { return DataDir() + L"\\history.json"; }

static std::vector<HistoryEntry> LoadLocked() {
    std::vector<HistoryEntry> out;
    std::wstring text;
    if (!ReadTextFile(PathOf(), text)) return out;
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Array) return out;
    for (auto& o : v.arr) {
        HistoryEntry e;
        e.time = (long long)o.GetNum(L"time", 0);
        e.key = o.GetStr(L"key");
        e.name = o.GetStr(L"name");
        e.from = o.GetStr(L"from");
        e.to = o.GetStr(L"to");
        e.result = o.GetStr(L"result");
        e.message = o.GetStr(L"message");
        e.overnight = o.GetBool(L"overnight", false);
        out.push_back(e);
    }
    return out;
}

void Add(const HistoryEntry& e) {
    std::lock_guard<std::mutex> g(s_lock);
    std::vector<HistoryEntry> all = LoadLocked();
    all.push_back(e);
    if (all.size() > kMax) all.erase(all.begin(), all.begin() + (all.size() - kMax));
    json::Value arr = json::Value::MakeArray();
    for (auto& x : all) {
        json::Value o = json::Value::MakeObject();
        o.Set(L"time", json::Value::Num((double)x.time));
        o.Set(L"key", json::Value::Str(x.key));
        o.Set(L"name", json::Value::Str(x.name));
        o.Set(L"from", json::Value::Str(x.from));
        o.Set(L"to", json::Value::Str(x.to));
        o.Set(L"result", json::Value::Str(x.result));
        if (!x.message.empty()) o.Set(L"message", json::Value::Str(x.message));
        if (x.overnight) o.Set(L"overnight", json::Value::Boolean(true));
        arr.arr.push_back(std::move(o));
    }
    std::wstring text;
    json::Write(arr, text);
    if (!WriteTextFileAtomic(PathOf(), text)) Log(L"couldn't save history.json");
}

std::vector<HistoryEntry> Load() {
    std::lock_guard<std::mutex> g(s_lock);
    return LoadLocked();
}

void Clear() {
    std::lock_guard<std::mutex> g(s_lock);
    DeleteFileW(PathOf().c_str());
}

} // namespace history
