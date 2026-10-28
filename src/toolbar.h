#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <vector>
#include "backdrop.h"
struct ToolbarState {
  bool pinned = false, subpixel = false;
  int petSize = 128, panelPercent = 115;
  float strength = .75f;
  BackdropMode backdrop = BackdropMode::Acrylic;
  bool spine=false;
};
void InitToolbar(HWND owner, ID2D1Factory *graphics, IDWriteFactory *text,
                 ToolbarState (*state)(), void (*action)(int),
                 HRESULT (*material)(HWND),
                 std::vector<RECT> (*obstacles)(), bool preview);
void ShowToolbar();
void CloseToolbar();
bool IsToolbarOpen();
void DestroyToolbar();
void RefreshToolbarPlacement();
void RefreshToolbarMaterial();
