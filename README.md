# Restock

One-click updates for every app on your PC that isn't part of the larosa023 suite. Think "Hangar for everything else": Restock finds what's out of date (7-Zip, VLC, Notepad++, Python, your drivers' companion apps…) and updates it with winget, while Hangar keeps your own apps current.

**Status: v0.1.0.** Updates, All apps, Self-updating, Pinned & ignored and History pages; Update all with a queue; closing and reopening open apps; skip, pin and ignore; a tray schedule with notifications and optional overnight updates; 14 themes plus custom theme files; and Restock updates itself.

## Run it

Download `Restock.exe` from [Releases](https://github.com/larosa023/Restock/releases) and double-click it, or install it from **Hangar**. It runs from anywhere and needs no admin rights. Windows may show SmartScreen the first time because the exe isn't signed yet: **More info → Run anyway**.

Portable mode: put an empty file named `portable` next to `Restock.exe`, and its data goes in a `data` folder beside it.

Restock uses **winget** (App Installer), which ships with Windows 11. If it's missing, Restock sets it up by itself.

## Using it

| | |
|---|---|
| **Updates** | Everything with a newer version: icon, installed → available version, and where it comes from (winget, Store). Apps Restock leaves out of Update all (pinned, skipped, self-updating) are listed below the rest |
| **All apps** | Every installed app, including the ones winget doesn't know (they come from Windows' Apps & features list) |
| **Update** | **Update all** or one app at a time. Updates run one after another, silently where the installer allows, with progress and a result for each: updated, failed (and why), or needs a restart |
| **Open apps** | If an app is open, Restock asks first, closes it the normal way, updates it and opens it again. It only ends an app by force if you say so |
| **Admin rights** | Asked for only when a specific installer needs them, one update at a time. Never at launch |
| **The ⋯ menu** | Skip this version · Pin (never update) · Ignore this app · Include a self-updating app · Open install folder · Copy winget ID. Right-click a row or press Shift+F10 for the same menu |
| **Self-updating apps** | Chrome, Edge, Firefox, Discord, Steam, Spotify, Store apps and others keep themselves current, so Restock marks them and leaves them alone. Settings → Include self-updating apps changes that |
| **Your suite** | Apps from Hangar's [manifest](https://github.com/larosa023/Hangar/blob/main/manifest.json) are left to Hangar. **Suite apps in Hangar** in the sidebar opens it |
| **Schedule** | From the tray icon: check daily or weekly, get a notification when updates are waiting, and optionally update everything overnight (2 to 5 AM, skipping apps that are open, pinned or installed for all users) |
| **History** | Every update with its date, the version before and after, and how it went |
| **Keyboard** | **Ctrl+U** updates all · **F5** checks again · **Ctrl+K** (or just type) searches · ↑↓ move · **Enter** updates the selected app · **Ctrl+1–5** switch pages · **Ctrl+H** History · **Ctrl+,** Settings · Esc goes back |

Closing the window keeps Restock in the tray so the schedule can run. **Quit Restock** is in the tray icon's menu. There's no background service, no account and no telemetry.

## Themes

The suite's 14 themes (Dark with Mica, Light, Windows, Midnight, Glass, Frost, Twilight, Forest, Sunset, Ocean, Rose, Graphite, Paper, Mist), accent colors, and custom `.json` theme files in the [Hangar format](https://github.com/larosa023/Hangar/blob/main/docs/THEMES.md). Themes in Restock's themes folder and in Hangar's both show up. Everything switches live.

## Updates to Restock itself

Restock checks GitHub Releases on launch and every 3 hours (Stable, or Beta for versions with a `-`), downloads in the background, verifies the SHA-256 against GitHub's digest and `SHA256SUMS.txt`, and shows **Restart to update**. With **Install Restock updates automatically** on, it does that by itself when you're not using it. If a new version doesn't start, Restock puts the previous one back and skips that version. **Settings → Restock updates** has **Check for updates now**, the channel, and rollback.

## Build

```bash
./build.sh                         # Linux/macOS, needs Zig: pip install ziglang
cmake -S . -B build -A x64 && cmake --build build --config Release   # Windows, VS 2022
g++ -std=c++17 -Isrc tests/parse_test.cpp src/wingetparse.cpp -o parse_test && ./parse_test   # winget parser tests
```

A release is published automatically when `RESTOCK_VERSION` in `src/common.h` changes on `main`. Versions containing a `-` are published as pre-releases (the Beta channel).

## Docs

- [Architecture: how Restock reads winget, runs the queue, and fits the suite](docs/ARCHITECTURE.md)
