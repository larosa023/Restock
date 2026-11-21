// Restock - running winget: finding it, listing installed apps and updates, updating one package,
// and installing winget itself when it's missing.
#pragma once
#include "common.h"
#include "wingetparse.h"
#include <functional>
#include <atomic>

namespace winget {

std::wstring Path();                         // "" when winget isn't installed
std::wstring Version();                      // "v1.9.25200", cached after the first call

// `winget list` and `winget upgrade` as rows. False (with a message) when winget couldn't run.
bool ListInstalled(std::vector<wg::Row>& rows, std::wstring* error);
bool ListUpgrades(bool includeUnknown, std::vector<wg::Row>& rows, std::wstring* error);
// Full IDs from `winget export`, to complete IDs that `list` shortened. (id, source)
bool ExportIds(std::vector<std::pair<std::wstring, std::wstring>>& ids);

struct UpgradeRequest {
    std::wstring id, source;
    bool includeUnknown = true;
    bool elevated = false;                   // run winget as administrator (one UAC prompt)
};
// Updates one package. `progress(percent, phase)` is called as output arrives (percent -1 when
// unknown). Setting `cancel` stops winget. Returns winget's exit code and output.
struct UpgradeOutput {
    bool started = false;
    bool declined = false;                   // the UAC prompt was declined (elevated only)
    bool cancelled = false;
    uint32_t exitCode = 0;
    std::wstring output;
};
UpgradeOutput Upgrade(const UpgradeRequest& req, const std::function<void(int, const std::wstring&)>& progress,
                      const std::atomic<bool>* cancel);

// Installs or repairs winget (App Installer): registers it if Windows already has it, otherwise
// downloads the latest release from microsoft/winget-cli, verifies it and installs it.
bool Install(const std::function<void(const std::wstring&)>& status, std::wstring* error);

} // namespace winget
