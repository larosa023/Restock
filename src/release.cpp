// Restock - GitHub releases
#include "release.h"
#include "util.h"
#include "net.h"
#include "json.h"
#include <algorithm>
#include <mutex>
#include <ctime>

bool ParseReleases(const std::string& body, std::vector<ReleaseInfo>& out, std::wstring* error) {
    json::Value root;
    if (!json::Parser(Utf8ToWide(body)).Parse(root) || root.type != json::Value::Array) {
        if (error) *error = L"GitHub sent an unexpected reply.";
        return false;
    }
    out.clear();
    for (const json::Value& r : root.arr) {
        if (r.GetBool(L"draft", false)) continue;
        ReleaseInfo info;
        info.tag = r.GetStr(L"tag_name");
        std::wstring v = NormalizeVersion(info.tag);
        if (v.empty() || !iswdigit(v[0])) continue;
        info.version = v;
        info.title = r.GetStr(L"name", info.tag);
        info.notes = r.GetStr(L"body");
        info.published = r.GetStr(L"published_at");
        info.page = r.GetStr(L"html_url");
        info.prerelease = r.GetBool(L"prerelease", false) || v.find(L'-') != std::wstring::npos;
        if (const json::Value* assets = r.Get(L"assets"); assets && assets->type == json::Value::Array) {
            for (const json::Value& a : assets->arr) {
                Asset as;
                as.name = a.GetStr(L"name");
                as.url = a.GetStr(L"browser_download_url");
                as.size = (uint64_t)a.GetNum(L"size", 0);
                std::wstring digest = a.GetStr(L"digest");
                if (IStartsWith(digest, L"sha256:")) as.sha256 = Lower(digest.substr(7));
                if (!as.name.empty() && !as.url.empty()) info.assets.push_back(as);
            }
        }
        out.push_back(std::move(info));
    }
    std::stable_sort(out.begin(), out.end(), [](const ReleaseInfo& a, const ReleaseInfo& b) { return CompareVersions(a.version, b.version) > 0; });
    return true;
}

const Asset* FindAsset(const ReleaseInfo& r, const std::wstring& name) {
    for (auto& a : r.assets) if (IEquals(a.name, name)) return &a;
    return nullptr;
}

std::wstring ExpectedHashFromSums(const std::string& sums, const std::wstring& assetName) {
    std::wstring text = Utf8ToWide(sums);
    size_t i = 0;
    std::wstring single;
    int lines = 0;
    while (i < text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = Trim(text.substr(i, e - i));
        i = e + 1;
        if (line.empty()) continue;
        ++lines;
        size_t sp = line.find_first_of(L" \t");
        std::wstring hash = Lower(line.substr(0, sp));
        if (hash.size() != 64 || hash.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) continue;
        std::wstring name = sp == std::wstring::npos ? L"" : Trim(line.substr(sp));
        if (!name.empty() && name[0] == L'*') name.erase(0, 1);
        if (IEquals(name, assetName)) return hash;
        if (name.empty()) single = hash;
    }
    return lines == 1 ? single : L"";
}

// ---------------------------------------------------------------- Hangar's release index
static std::mutex s_indexLock;
static json::Value s_index;
static bool s_indexLoaded = false;
static long long s_indexFetchedAt = 0;

static std::wstring IndexUrl() {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"RESTOCK_INDEX_URL", buf, 1024);
    if (n > 0 && n < 1024) return buf;
    return L"https://raw.githubusercontent.com/larosa023/Hangar/index/releases.json";
}

static bool LoadIndexText(const std::string& body) {
    json::Value v;
    if (!json::Parser(Utf8ToWide(body)).Parse(v) || v.type != json::Value::Object || !v.Get(L"repos")) return false;
    s_index = std::move(v);
    s_indexLoaded = true;
    return true;
}

bool IndexReleases(const std::wstring& repo, long long maxAgeSecs, std::string& body) {
    std::lock_guard<std::mutex> g(s_indexLock);
    long long now = (long long)time(nullptr);
    std::wstring cachePath = CacheDir() + L"\\index.json", etagPath = CacheDir() + L"\\index.etag";
    if (!s_indexLoaded) {
        std::string cached;
        if (ReadFileBytes(cachePath, cached)) LoadIndexText(cached);
    }
    if (now - s_indexFetchedAt >= maxAgeSecs) {
        std::wstring etag;
        if (s_indexLoaded) ReadTextFile(etagPath, etag);
        net::Response r;
        if (net::Get(IndexUrl(), r, Trim(etag))) {
            if (r.status == 200 && LoadIndexText(r.body)) {
                WriteFileAtomic(cachePath, r.body);
                WriteTextFileAtomic(etagPath, r.etag);
                s_indexFetchedAt = now;
            } else if (r.status == 304) {
                s_indexFetchedAt = now;
            } else {
                Log(L"release index: HTTP %d", r.status);
                s_indexFetchedAt = now - maxAgeSecs + 60;
            }
        } else {
            Log(L"release index: %ls", r.error.c_str());
            s_indexFetchedAt = now - maxAgeSecs + 60;
        }
    }
    if (!s_indexLoaded) return false;
    long long generated = (long long)s_index.GetNum(L"generated", 0);
    if (generated && now - generated > 24 * 3600) return false;   // the workflow stopped refreshing it
    const json::Value* repos = s_index.Get(L"repos");
    if (!repos || repos->type != json::Value::Object) return false;
    const json::Value* rels = repos->Get(Lower(repo));
    if (!rels || rels->type != json::Value::Array) return false;
    std::wstring text;
    json::Write(*rels, text);
    body = WideToUtf8(text);
    return true;
}
