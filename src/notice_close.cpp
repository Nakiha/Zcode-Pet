#include <windows.h>
#include "notice_close.h"
#include "interaction_motion.h"
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
  bool closeRequested = false, retiring = false;
};
std::map<HWND, std::unique_ptr<CloseButton>> buttons;

void SyncFeedback(CloseButton &state) {
  if (state.retiring)
    return;
  POINT point;
  GetCursorPos(&point);
  const auto under = WindowFromPoint(point);
  ScreenToClient(state.button, &point);
  const float x = state.scale > 0 ? point.x / state.scale - 12 : 100,
              y = state.scale > 0 ? point.y / state.scale - 12 : 100;
  const bool hot = IsWindowVisible(state.button) && x * x + y * y <= 144 &&
                   (under == state.button || GetCapture() == state.button);
  const bool down = hot && state.pressed;
  SetInteractionFeedback(state.button, L"close", {hot, down},
                         down  ? 2.f
                         : hot ? 1.f
                               : 0);
}
Gdiplus::Color FeedbackColor(float amount, const Gdiplus::Color &normal,
                             const Gdiplus::Color &hover,
                             const Gdiplus::Color &pressed) {
  const auto a = amount > 1 ? hover : normal, b = amount > 1 ? pressed : hover;
  const float t = std::clamp(amount > 1 ? amount - 1 : amount, 0.f, 1.f);
  auto channel = [&](BYTE from, BYTE to) {
    return (BYTE)std::lround(from + (to - from) * t);
  };
  return {channel(a.GetA(), b.GetA()), channel(a.GetR(), b.GetR()),
          channel(a.GetG(), b.GetG()), channel(a.GetB(), b.GetB())};
}

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
      const float feedback = InteractionOpacity(button, L"close");
      Gdiplus::SolidBrush fill(FeedbackColor(
          feedback, {248, 78, 70, 91}, {255, 104, 89, 123}, {255, 56, 49, 68}));
      Gdiplus::Pen edge(FeedbackColor(feedback, {170, 232, 222, 244},
                                      {230, 247, 238, 255},
                                      {180, 216, 201, 234}),
                        .85f);
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
  if (state->retiring &&
      (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
       message == WM_MOUSEMOVE || message == WM_MOUSELEAVE ||
       message == WM_CAPTURECHANGED))
    return 0;
  switch (message) {
  case WM_PAINT: {
    PAINTSTRUCT paint;
    BeginPaint(h, &paint);
    Paint(*state, state->scale);
    EndPaint(h, &paint);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_MOUSEMOVE: {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
    TrackMouseEvent(&track);
    SyncFeedback(*state);
    return 0;
  }
  case WM_MOUSELEAVE:
    SetInteractionFeedback(h, L"close", {}, 0);
    return 0;
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_LBUTTONDOWN:
    state->pressed = true;
    SetCapture(h);
    SyncFeedback(*state);
    return 0;
  case WM_LBUTTONUP: {
    const bool wasPressed = state->pressed;
    const float x = GET_X_LPARAM(l) / state->scale - 12,
                y = GET_Y_LPARAM(l) / state->scale - 12;
    state->pressed = false;
    ReleaseCapture();
    SyncFeedback(*state);
    if (wasPressed && x * x + y * y <= 144) {
      state->closeRequested = true;
      PostMessageW(state->owner, WM_CLOSE, 0, 0);
    }
    return 0;
  }
  case WM_CAPTURECHANGED:
    state->pressed = false;
    SyncFeedback(*state);
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
  if (state.retiring)
    visible = IsWindowVisible(notice) != FALSE;
  if (!visible) {
    if (IsWindowVisible(button))
      ShowWindow(button, SW_HIDE);
    state.pressed = false;
    state.retiring = false;
    if (GetCapture() == button)
      ReleaseCapture();
    ClearInteractionFeedback(button);
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
      current.bottom - current.top == side) {
    SyncFeedback(state);
    return;
  }
  SetWindowPos(button, HWND_TOPMOST, x, y, side, side,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SyncFeedback(state);
}
bool IsNoticeClose(HWND notice, HWND window) {
  auto found = buttons.find(notice);
  return found != buttons.end() && found->second->button == window;
}
void DestroyNoticeClose(HWND notice) {
  auto found = buttons.find(notice);
  if (found == buttons.end())
    return;
  ClearInteractionFeedback(found->second->button);
  if (found->second->button)
    DestroyWindow(found->second->button);
  buttons.erase(found);
}
void RetireNoticeClose(HWND notice) {
  auto found = buttons.find(notice);
  if (found == buttons.end())
    return;
  auto &state = *found->second;
  if (state.closeRequested && IsWindowVisible(state.button)) {
    state.retiring = true;
    state.pressed = false;
    SetInteractionFeedback(state.button, L"close", {true, true}, 2.f);
  } else
    ShowNoticeClose(notice, state.scale, false);
}
