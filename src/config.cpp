// Restock - settings
#include "config.h"
#include "util.h"
#include "json.h"

Config g_cfg;

static std::wstring ConfigPath() { return DataDir() + L"\\settings.json"; }

static void ReadSet(const json::Value& v, const wchar_t* key, std::set<std::wstring>& out) {
    if (const json::Value* a = v.Get(key); a && a->type == json::Value::Array)
        for (auto& x : a->arr) if (x.type == json::Value::String && !x.s.empty()) out.insert(x.s);
}

static json::Value WriteSet(const std::set<std::wstring>& s) {
    json::Value a = json::Value::MakeArray();
    for (auto& x : s) a.arr.push_back(json::Value::Str(x));
    return a;
}

void ConfigLoad() {
    std::wstring text;
    if (!ReadTextFile(ConfigPath(), text)) return;
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) { Log(L"settings.json is damaged; using defaults"); return; }
    Config c;
    c.theme = v.GetStr(L"theme", c.theme);
    c.accent = v.GetStr(L"accent", c.accent);
    c.page = v.GetStr(L"page", c.page);
    c.schedule = v.GetStr(L"schedule", c.schedule);
    if (c.schedule != L"off" && c.schedule != L"daily" && c.schedule != L"weekly") c.schedule = L"daily";
    c.notify = v.GetBool(L"notify", c.notify);
    c.nightly = v.GetBool(L"nightly", c.nightly);
    c.tray = v.GetBool(L"tray", c.tray);
    c.startup = v.GetBool(L"startup", c.startup);
    c.lastCheck = (long long)v.GetNum(L"lastCheck", 0);
    c.lastNightly = (long long)v.GetNum(L"lastNightly", 0);
    c.notified = v.GetStr(L"notified");
    c.trayHintShown = v.GetBool(L"trayHintShown", false);
    c.includeSelfUpdating = v.GetBool(L"includeSelfUpdating", c.includeSelfUpdating);
    c.includeUnknown = v.GetBool(L"includeUnknown", c.includeUnknown);
    c.reopen = v.GetBool(L"reopen", c.reopen);
    ReadSet(v, L"ignored", c.ignored);
    ReadSet(v, L"pinned", c.pinned);
    ReadSet(v, L"forceInclude", c.forceInclude);
    if (const json::Value* sk = v.Get(L"skipped"); sk && sk->type == json::Value::Object)
        for (auto& kv : sk->obj) if (kv.second.type == json::Value::String) c.skipped[kv.first] = kv.second.s;
    c.selfBeta = v.GetBool(L"selfBeta", c.selfBeta);
    c.selfAuto = v.GetBool(L"selfAuto", c.selfAuto);
    c.skipVersion = v.GetStr(L"skipVersion");
    if (const json::Value* w = v.Get(L"window"); w && w->type == json::Value::Object) {
        c.windowX = (int)w->GetNum(L"x", CW_USEDEFAULT);
        c.windowY = (int)w->GetNum(L"y", CW_USEDEFAULT);
        c.windowW = (int)w->GetNum(L"w", 0);
        c.windowH = (int)w->GetNum(L"h", 0);
        c.maximized = w->GetBool(L"maximized", false);
    }
    g_cfg = c;
}

void ConfigSave() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"theme", json::Value::Str(g_cfg.theme));
    v.Set(L"accent", json::Value::Str(g_cfg.accent));
    v.Set(L"page", json::Value::Str(g_cfg.page));
    v.Set(L"schedule", json::Value::Str(g_cfg.schedule));
    v.Set(L"notify", json::Value::Boolean(g_cfg.notify));
    v.Set(L"nightly", json::Value::Boolean(g_cfg.nightly));
    v.Set(L"tray", json::Value::Boolean(g_cfg.tray));
    v.Set(L"startup", json::Value::Boolean(g_cfg.startup));
    v.Set(L"lastCheck", json::Value::Num((double)g_cfg.lastCheck));
    v.Set(L"lastNightly", json::Value::Num((double)g_cfg.lastNightly));
    v.Set(L"notified", json::Value::Str(g_cfg.notified));
    v.Set(L"trayHintShown", json::Value::Boolean(g_cfg.trayHintShown));
    v.Set(L"includeSelfUpdating", json::Value::Boolean(g_cfg.includeSelfUpdating));
    v.Set(L"includeUnknown", json::Value::Boolean(g_cfg.includeUnknown));
    v.Set(L"reopen", json::Value::Boolean(g_cfg.reopen));
    v.Set(L"ignored", WriteSet(g_cfg.ignored));
    v.Set(L"pinned", WriteSet(g_cfg.pinned));
    v.Set(L"forceInclude", WriteSet(g_cfg.forceInclude));
    json::Value sk = json::Value::MakeObject();
    for (auto& kv : g_cfg.skipped) sk.Set(kv.first, json::Value::Str(kv.second));
    v.Set(L"skipped", sk);
    v.Set(L"selfBeta", json::Value::Boolean(g_cfg.selfBeta));
    v.Set(L"selfAuto", json::Value::Boolean(g_cfg.selfAuto));
    v.Set(L"skipVersion", json::Value::Str(g_cfg.skipVersion));
    json::Value w = json::Value::MakeObject();
    w.Set(L"x", json::Value::Num(g_cfg.windowX));
    w.Set(L"y", json::Value::Num(g_cfg.windowY));
    w.Set(L"w", json::Value::Num(g_cfg.windowW));
    w.Set(L"h", json::Value::Num(g_cfg.windowH));
    w.Set(L"maximized", json::Value::Boolean(g_cfg.maximized));
    v.Set(L"window", w);
    std::wstring out;
    json::Write(v, out);
    if (!WriteTextFileAtomic(ConfigPath(), out)) Log(L"couldn't save settings.json");
}

static std::vector<std::wstring> SkipList() {
    std::vector<std::wstring> out;
    size_t i = 0;
    const std::wstring& s = g_cfg.skipVersion;
    while (i < s.size()) {
        size_t e = s.find(L',', i);
        if (e == std::wstring::npos) e = s.size();
        std::wstring v = Trim(s.substr(i, e - i));
        if (!v.empty()) out.push_back(v);
        i = e + 1;
    }
    return out;
}

static void SetSkipList(const std::vector<std::wstring>& list) {
    g_cfg.skipVersion.clear();
    for (auto& v : list) g_cfg.skipVersion += (g_cfg.skipVersion.empty() ? L"" : L",") + v;
}

bool IsSkippedVersion(const std::wstring& v) {
    for (auto& s : SkipList()) if (CompareVersions(s, v) == 0) return true;
    return false;
}

void SkipVersion(const std::wstring& v) {
    if (IsSkippedVersion(v)) return;
    auto list = SkipList();
    list.push_back(NormalizeVersion(v));
    SetSkipList(list);
}

void PruneSkippedVersions(const std::wstring& current) {
    auto list = SkipList();
    std::vector<std::wstring> keep;
    for (auto& v : list) if (CompareVersions(v, current) > 0) keep.push_back(v);
    SetSkipList(keep);
}
