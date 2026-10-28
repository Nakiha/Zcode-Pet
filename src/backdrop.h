#pragma once
#include <windows.h>
#include <dwmapi.h>

enum class BackdropMode { Acrylic, Mica, Solid };
inline BackdropMode ReadBackdropMode(int value) {
  return value == 1   ? BackdropMode::Mica
         : value == 2 ? BackdropMode::Solid
                      : BackdropMode::Acrylic;
}
inline DWM_SYSTEMBACKDROP_TYPE BackdropType(BackdropMode mode) {
  return mode == BackdropMode::Mica    ? DWMSBT_MAINWINDOW
         : mode == BackdropMode::Solid ? DWMSBT_NONE
                                       : DWMSBT_TRANSIENTWINDOW;
}
inline const wchar_t *BackdropName(BackdropMode mode) {
  return mode == BackdropMode::Mica    ? L"壁纸云母"
         : mode == BackdropMode::Solid ? L"纯色"
                                       : L"实时亚克力";
}
inline constexpr UINT BackdropChanged = WM_APP + 35;
