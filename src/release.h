// Restock - GitHub releases, for Restock's own updates. Same conventions as the rest of the suite:
// the tag is the version ("v0.1.0"), a version with a "-" or a pre-release is Beta, drafts are ignored.
#pragma once
#include "common.h"

struct Asset {
    std::wstring name, url, sha256;     // sha256 from GitHub's asset digest (may be empty)
    uint64_t size = 0;
};

struct ReleaseInfo {
    std::wstring tag, version, title, notes, published, page;
    bool prerelease = false;
    std::vector<Asset> assets;
};

// Parses a /releases array, newest version first
bool ParseReleases(const std::string& body, std::vector<ReleaseInfo>& out, std::wstring* error);
const Asset* FindAsset(const ReleaseInfo& r, const std::wstring& name);
// Finds an asset's hash in a SHA256SUMS-style file ("<hex>  <name>" lines). "" if absent.
std::wstring ExpectedHashFromSums(const std::string& sums, const std::wstring& assetName);

// Hangar's release index (releases.json on its `index` branch): every suite app's releases in one file
// that isn't subject to GitHub's 60-requests-an-hour limit. False if it doesn't cover `repo`.
bool IndexReleases(const std::wstring& repo, long long maxAgeSecs, std::string& body);
