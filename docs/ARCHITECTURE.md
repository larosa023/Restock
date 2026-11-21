# Restock architecture

## 1. Tech stack

**Native C++17, Win32 + Direct2D/DirectWrite, cross-compiled with Zig**: the same stack, toolchain and shared code as Hangar (`util`, `json`, `net`, `gfx`, `themes`, `selfupdate` started as Hangar's files). One exe of about 900 KB with no runtime, a window that opens instantly, and about 15 to 25 MB of RAM while open. Hidden in the tray, it frees its GPU resources and text caches and trims its working set.

## 2. Where the data comes from

| Source | What it gives |
|---|---|
| `winget list` | Every installed app winget knows about: name, ID, version, source |
| `winget upgrade [--include-unknown]` | Which of them have a newer version, and which version |
| `winget export --include-versions` | Full IDs, used only to complete IDs an older winget shortened with "…" |
| Apps & features (`Uninstall` keys in HKCU and both HKLM views) | Publisher, install folder, icon, "installed for all users", and the apps winget doesn't know at all |

winget has no machine-readable output for `list` and `upgrade`, so `src/wingetparse.cpp` reads its tables: column starts come from the header line, measured in console cells (East Asian characters take two), a header word only counts as a column if some row's value starts exactly there (so a two-word header in another language isn't split), and a row must line up with the column boundaries (so "2 upgrades available." isn't read as a row). It has no Windows dependencies and is unit-tested in `tests/parse_test.cpp`, which CI runs before every build.

The merged result is cached in `cache\inventory.json`. The window opens from that cache, then a fresh scan runs in the background.

## 3. Updating

- **The queue** (`jobs.cpp`): one update at a time on its own thread, because installers (MSI in particular) don't like to overlap. Each step reports progress to the window: waiting, closing, downloading (winget's `12.5 MB / 40 MB` lines become a percentage), installing.
- **Command:** `winget upgrade --id <id> --exact --source <source> --silent --accept-package-agreements --accept-source-agreements --disable-interactivity --include-unknown`.
- **Results:** winget's exit code first (`wingetparse.cpp → Classify`), its output second. Updated, already up to date, needs a restart (3010/1641 and winget's reboot codes), app in use, needs administrator rights, cancelled, or failed with the installer's own reason. winget's full output for failures goes to `restock.log`.
- **Open apps:** an app's processes are the ones running from its install folder (never a shared folder like Program Files itself). Restock asks before closing them, sends `WM_CLOSE` to their windows, waits 10 seconds, and only ends them by force if you agree. Afterwards it reopens the exe that owned the visible window.
- **Administrator rights:** Restock runs as you. When an installer can't run that way (winget's "requires admin" or an access-denied result), Restock asks, then runs that one `winget upgrade` elevated through `cmd /c … > file` (an elevated process can't write to our pipe). Overnight runs never ask: they skip apps installed for all users.
- **Self-updating apps:** Microsoft Store apps, Squirrel installs (an `Update.exe` beside the app), and a list of known IDs (browsers, game launchers, chat apps, VS Code…). They're marked and left out of Update all unless you include them, globally or per app.
- **History:** every finished update is appended to `history.json` (newest 2,000 kept).

## 4. The suite

Restock reads Hangar's `manifest.json` (fetched with an ETag and cached; a copy is built into the exe, refreshed by CI at build time). An installed app is a suite app when its winget ID starts with `larosa023.`, its publisher names larosa, its install folder is a suite app's portable folder or Velopack folder, or its name is a suite app's name and its publisher is empty or the app's own name (so another company's "Prism" isn't hidden). Suite apps aren't listed; the sidebar links to Hangar instead. Restock is in Hangar's manifest itself (`portable`, `args.quit: --quit`).

## 5. The tray and the schedule

There's no service. The tray icon (the same process) runs a timer every 5 minutes:
- **Check:** daily or weekly since the last check. A background check that finds updates shows one Windows notification per set of updates.
- **Overnight:** between 2 and 5 AM, when nobody has used the PC for 10 minutes, at most once a night: check, then queue everything eligible that won't need you.
- **Start with Windows:** `HKCU\…\Run` → `Restock.exe --tray`, which starts with no window.
- `Restock.exe --quit` asks a running Restock to exit for real; Hangar runs it before updating Restock.

## 6. Restock's own updates

The same design as Hangar's (`selfupdate.cpp`): check `larosa023/Restock` releases (Hangar's release index first, then the GitHub API), Stable or Beta, download `Restock.exe` to `updates\<version>\`, verify SHA-256 against GitHub's digest and `SHA256SUMS.txt`, then swap the exe, start the new copy with `--after-update`, and wait for its "healthy" signal. No signal within 20 seconds puts the old exe back and skips that version. The previous 2 versions are kept for rollback.

## 7. Files

```
Restock/
├─ src/
│  ├─ main.cpp          single instance, --tray, --quit
│  ├─ ui.cpp            the window: pages, rows, settings, themes, tray handling, schedule
│  ├─ jobs.cpp          background work: scans, icons, process polls, the update queue
│  ├─ inventory.cpp     winget + Apps & features merged, suite filter, self-updating list, icons, processes
│  ├─ winget.cpp        running winget, elevated runs, installing winget
│  ├─ wingetparse.cpp   reading winget's tables and results (portable, unit-tested)
│  ├─ history.cpp       history.json
│  ├─ tray.cpp          tray icon, notifications, Start with Windows
│  ├─ selfupdate.cpp    Restock's own updater
│  ├─ release.cpp       GitHub releases and Hangar's release index
│  └─ config, util, net, json, gfx, themes   shared with Hangar
├─ res/                 icon, Windows manifest, version info, built-in copy of Hangar's manifest
└─ tests/parse_test.cpp
```

Data lives in `%LOCALAPPDATA%\Restock\` (or `.\data\` in portable mode): `settings.json`, `history.json`, `restock.log`, `cache\`, `themes\`, `updates\`, `versions\`.
