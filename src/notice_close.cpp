#include <windows.h>
#include "notice_close.h"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <algorithm>
#include <map>
#include <memory>

namespace {
struct CloseButton {
  HWND button = nullptr, owner = nullptr;
  float scale = 0;
  bool pressed = false;
};
std::map<HWND, std::unique_ptr<CloseButton>> buttons;

void Paint(CloseButton &state, float dpi) {
  const auto button = state.button;
  if (!button || dpi <= 0)
    return;
  const int side = std::max(1, (int)(24 * dpi));
  HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB};
  void *bits = nullptr;
  HBITMAP image =
      CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (image && bits) {
    auto previous = SelectObject(memory, image);
    {
      Gdiplus::Bitmap bitmap(side, side, side * 4, PixelFormat32bppPARGB,
                             static_cast<BYTE *>(bits));
      Gdiplus::Graphics g(&bitmap);
      g.Clear(Gdiplus::Color(0, 0, 0, 0));
      g.ScaleTransform(dpi, dpi);
      g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      Gdiplus::SolidBrush fill(state.pressed ? Gdiplus::Color(255, 56, 49, 68)
                                             : Gdiplus::Color(248, 78, 70, 91));
      Gdiplus::Pen edge(Gdiplus::Color(170, 232, 222, 244), .85f);
      Gdiplus::Pen cross(Gdiplus::Color(255, 255, 255, 255), 1.8f);
      cross.SetStartCap(Gdiplus::LineCapRound);
      cross.SetEndCap(Gdiplus::LineCapRound);
      g.FillEllipse(&fill, 2.f, 2.f, 20.f, 20.f);
      g.DrawEllipse(&edge, 2.f, 2.f, 20.f, 20.f);
      g.DrawLine(&cross, 8.5f, 8.5f, 15.5f, 15.5f);
      g.DrawLine(&cross, 15.5f, 8.5f, 8.5f, 15.5f);
    }
    POINT origin{};
    SIZE size{side, side};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(button, screen, nullptr, &size, memory, &origin, 0,
                        &blend, ULW_ALPHA);
    SelectObject(memory, previous);
  }
  if (image)
    DeleteObject(image);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  state.scale = dpi;
}

LRESULT CALLBACK Proc(HWND h, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) {
    const auto create = reinterpret_cast<CREATESTRUCTW *>(l);
    SetWindowLongPtrW(h, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto state =
      reinterpret_cast<CloseButton *>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (!state)
    return DefWindowProcW(h, message, w, l);
  switch (message) {
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_LBUTTONDOWN:
    state->pressed = true;
    SetCapture(h);
    Paint(*state, state->scale);
    return 0;
  case WM_LBUTTONUP: {
    const bool wasPressed = state->pressed;
    const float x = GET_X_LPARAM(l) / state->scale - 12,
                y = GET_Y_LPARAM(l) / state->scale - 12;
    state->pressed = false;
    ReleaseCapture();
    Paint(*state, state->scale);
    if (wasPressed && x * x + y * y <= 144)
      PostMessageW(state->owner, WM_CLOSE, 0, 0);
    return 0;
  }
  case WM_CAPTURECHANGED:
    state->pressed = false;
    Paint(*state, state->scale);
    return 0;
  }
  return DefWindowProcW(h, message, w, l);
}
} // namespace

void InitNoticeClose(HWND notice, bool preview) {
  auto state = std::make_unique<CloseButton>();
  state->owner = notice;
  WNDCLASSW wc{};
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
  wc.lpfnWndProc = Proc;
  wc.lpszClassName = L"ZCodePet.NoticeClose";
  RegisterClassW(&wc);
  state->button = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOPMOST |
          (preview ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE),
      wc.lpszClassName, L"关闭提醒", WS_POPUP, 0, 0, 24, 24, notice, nullptr,
      wc.hInstance, state.get());
  buttons.emplace(notice, std::move(state));
}
void ShowNoticeClose(HWND notice, float dpi, bool visible) {
  auto found = buttons.find(notice);
  if (found == buttons.end())
    return;
  auto &state = *found->second;
  const auto button = state.button;
  if (!button)
    return;
  if (!visible) {
    if (IsWindowVisible(button))
      ShowWindow(button, SW_HIDE);
    return;
  }
  if (state.scale != dpi)
    Paint(state, dpi);
  RECT bounds;
  GetWindowRect(notice, &bounds);
  const int x = bounds.left - (int)(12 * dpi), y = bounds.top - (int)(12 * dpi),
            side = (int)(24 * dpi);
  RECT current;
  GetWindowRect(button, &current);
  if (IsWindowVisible(button) && current.left == x && current.top == y &&
      current.right - current.left == side &&
      current.bottom - current.top == side)
    return;
  SetWindowPos(button, HWND_TOPMOST, x, y, side, side,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
bool IsNoticeClose(HWND notice, HWND window) {
  auto found = buttons.find(notice);
  return found != buttons.end() && found->second->button == window;
}
void DestroyNoticeClose(HWND notice) {
  auto found = buttons.find(notice);
  if (found == buttons.end())
    return;
  if (found->second->button)
    DestroyWindow(found->second->button);
  buttons.erase(found);
}
