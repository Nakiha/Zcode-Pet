#pragma once
#include <windows.h>

inline void KeepRectangularPetHitArea(void *pixels, size_t count) {
  auto data = static_cast<DWORD *>(pixels);
  // WS_EX_LAYERED 的 alpha=0 像素会直接穿透，WM_NCHITTEST 无法补回。
  // 最低非零 alpha 稳定整个矩形的命中；不能随角色当前轮廓改变交互区。
  for (size_t i = 0; i < count; ++i)
    if ((data[i] & 0xff000000) == 0)
      data[i] = 0x01000000;
}

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
    info.bmiHeader = {
        sizeof(BITMAPINFOHEADER), requested, -requested, 1, 32, BI_RGB};
    bitmap =
        CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap || !pixels) {
      Reset();
      return false;
    }
    previous = SelectObject(dc, bitmap);
    side = requested;
    return true;
  }
};
