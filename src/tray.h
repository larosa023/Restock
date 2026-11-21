// Restock - the tray icon (it runs the schedule; there's no background service) and
// "Start with Windows".
#pragma once
#include "common.h"

namespace tray {

enum Command : UINT { CmdOpen = 1, CmdCheck, CmdUpdateAll, CmdHistory, CmdSettings, CmdQuit };

void Add(HWND hwnd, const std::wstring& tip);   // also call it again after Explorer restarts
void SetTip(const std::wstring& tip);
void Remove();
bool Shown();
void Notify(const std::wstring& title, const std::wstring& text);
UINT Menu(HWND hwnd, int updates, bool busy);    // returns a Command (0 if dismissed)
UINT TaskbarCreatedMessage();

void SetStartWithWindows(bool on);              // HKCU Run key: "Restock.exe" --tray
bool StartsWithWindows();

} // namespace tray
