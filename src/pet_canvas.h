#pragma once
#include <windows.h>

struct PetCanvasBuffer {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  void *pixels = nullptr;
  int side = 0;
  ~PetCanvasBuffer() { Reset(); }
  void Reset() {
    if (dc && previous)
      SelectObject(dc, previous);
    if (bitmap)
      DeleteObject(bitmap);
    if (dc)
      DeleteDC(dc);
    dc = nullptr;
    bitmap = nullptr;
    previous = nullptr;
    pixels = nullptr;
    side = 0;
  }
  bool Ensure(HDC screen, int requested) {
    if (dc && side == requested)
      return true;
    Reset();
    dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), requested, -requested,
                       1, 32, BI_RGB};
    bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap || !pixels) {
      Reset();
      return false;
    }
    previous = SelectObject(dc, bitmap);
    side = requested;
    return true;
  }
};
