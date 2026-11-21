// Restock - main window
#pragma once
#include "common.h"
#include "selfupdate.h"

// startHidden: launched with --tray (Start with Windows); the window is created but not shown
HWND UiCreate(HINSTANCE inst, int show, const selfupdate::AfterUpdate& after, bool startHidden);
