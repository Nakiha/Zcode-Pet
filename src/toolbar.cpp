#include <windows.h>
#include "toolbar.h"
#include "theme.h"
#include "floating_layout.h"
#include "popup_motion.h"
#include "ui_renderer.h"
#include "interaction_motion.h"
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
HWND window, owner;
ComPtr<ID2D1Factory> graphics;
ComPtr<IDWriteFactory> text;
ComPtr<ID2D1HwndRenderTarget> canvas;
ToolbarState (*getState)();
void (*execute)(int);
HRESULT (*applyMaterial)(HWND);
std::vector<RECT> (*getObstacles)();
float scale = 1;
bool opened = false, keyMode = false;
int page = 0, hovered = -1, pressed = -1, focus = 0;
POINT destination{};
HRESULT materialResult = E_PENDING;
constexpr float Cell = 38, Inset = 8, Height = 64;
struct Item {
  int id;
  const wchar_t *name;
};
std::vector<Item> Items() {
  if (page == 1)
    return {{1000, L"返回"},
            {10, L"宠物缩小"},
            {0, L"宠物当前尺寸"},
            {11, L"宠物放大"},
            {16, L"恢复宠物默认大小"}};
  if (page == 2)
    return {{1000, L"返回"},
            {12, L"面板缩小"},
            {0, L"面板当前缩放"},
            {13, L"面板放大"},
            {17, L"恢复面板默认大小"}};
  if (page == 3)
    return {{1000, L"返回"},
            {20, L"DirectWrite 灰度文字"},
            {21, L"亚克力子像素文字（实验）"},
            {5, L"子像素强度 50%"},
            {6, L"子像素强度 75%"},
            {7, L"子像素强度 100%"}};
  return {{1, L"显示或收起会话"}, {15, L"固定会话面板"},
          {100, L"宠物大小"},     {101, L"面板大小"},
          {102, L"文字显示"},     {14, L"恢复所有默认大小"},
          {3, L"素材来源与许可"}, {4, L"退出桌宠"}};
}
float Width() { return Inset * 2 + Cell * Items().size(); }
int Hit(float x, float y) {
  if (y < 4 || y > 42 || x < Inset)
    return -1;
  int i = (int)((x - Inset) / Cell);
  return i < (int)Items().size() ? i : -1;
}
void Label(const std::wstring &value, float x, float y, float width, float size,
           D2D1_COLOR_F color, bool center = false) {
  ComPtr<IDWriteTextFormat> font;
  text->CreateTextFormat(L"Segoe UI Variable", nullptr,
                         DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &font);
  if (!font)
    return;
  font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  if (center)
    font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  ComPtr<IDWriteInlineObject> sign;
  text->CreateEllipsisTrimmingSign(font.Get(), &sign);
  font->SetTrimming(&trim, sign.Get());
  ComPtr<ID2D1SolidColorBrush> brush;
  canvas->CreateSolidColorBrush(color, &brush);
  canvas->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  canvas->DrawTextW(value.c_str(), (UINT32)value.size(), font.Get(),
                    D2D1::RectF(x, y, x + width, y + 24), brush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Icon(int id, float x, float y, ID2D1SolidColorBrush *b) {
  auto line = [&](float a, float c, float d, float e) {
    canvas->DrawLine(D2D1::Point2F(x + a, y + c), D2D1::Point2F(x + d, y + e),
                     b, 1.55f);
  };
  auto circle = [&](float a, float c, float r) {
    canvas->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + a, y + c), r, r), b,
                        1.5f);
  };
  if (id == 1) {
    for (int i = -1; i <= 1; ++i) {
      line(-4, i * 6.f, 8, i * 6.f);
      canvas->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(x - 8, y + i * 6), 1.2f, 1.2f), b);
    }
    return;
  }
  if (id == 15) {
    line(-6, -7, 6, -7);
    line(-4, -7, -4, 0);
    line(4, -7, 4, 0);
    line(-4, 0, -7, 4);
    line(4, 0, 7, 4);
    line(-7, 4, 7, 4);
    line(0, 4, 0, 10);
    return;
  }
  if (id == 100) {
    canvas->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + 1), 8, 6), b, 1.5f);
    line(-7, -2, -7, -8);
    line(-7, -8, -2, -5);
    line(7, -2, 7, -8);
    line(7, -8, 2, -5);
    circle(-3, 1, .6f);
    circle(3, 1, .6f);
    line(-1, 4, 1, 4);
    return;
  }
  if (id == 101) {
    canvas->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x - 9, y - 7, x + 9, y + 7), 2, 2), b,
        1.5f);
    line(-3, -7, -3, 7);
    return;
  }
  if (id == 14 || id == 16 || id == 17) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(graphics->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
      return;
    // Feed an open circular arc into a solid arrowhead. The filled head stays
    // legible at toolbar size instead of reading as an L-shaped corner.
    sink->BeginFigure(D2D1::Point2F(x - 6.062f, y - 2.5f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(x, y - 6), D2D1::SizeF(7, 7), 0,
        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close()))
      return;
    auto properties = D2D1::StrokeStyleProperties();
    properties.startCap = properties.endCap = D2D1_CAP_STYLE_ROUND;
    properties.lineJoin = D2D1_LINE_JOIN_ROUND;
    ComPtr<ID2D1StrokeStyle> stroke;
    graphics->CreateStrokeStyle(properties, nullptr, 0, &stroke);
    canvas->DrawGeometry(path.Get(), b, 1.55f, stroke.Get());
    ComPtr<ID2D1PathGeometry> head;
    ComPtr<ID2D1GeometrySink> headSink;
    if (SUCCEEDED(graphics->CreatePathGeometry(&head)) &&
        SUCCEEDED(head->Open(&headSink))) {
      headSink->BeginFigure(D2D1::Point2F(x - 4.6f, y - 6), D2D1_FIGURE_BEGIN_FILLED);
      headSink->AddLine(D2D1::Point2F(x + .5f, y - 8.6f));
      headSink->AddLine(D2D1::Point2F(x + .5f, y - 3.4f));
      headSink->EndFigure(D2D1_FIGURE_END_CLOSED);
      if (SUCCEEDED(headSink->Close()))
        canvas->FillGeometry(head.Get(), b);
    }
    return;
  }
  if (id == 3) {
    circle(0, 0, 8);
    line(0, -1, 0, 5);
    circle(0, -4, .65f);
    return;
  }
  if (id == 4) {
    ComPtr<ID2D1PathGeometry> path;
    graphics->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(x - 5, y - 5), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(
        D2D1::Point2F(x + 5, y - 5), D2D1::SizeF(8, 8), 0,
        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    canvas->DrawGeometry(path.Get(), b, 1.6f);
    line(0, -9, 0, 0);
    return;
  }
  if (id == 1000) {
    line(2, -7, -5, 0);
    line(-5, 0, 2, 7);
    return;
  }
  if (id == 10 || id == 12 || id == 11 || id == 13) {
    line(-7, 0, 7, 0);
    if (id == 11 || id == 13)
      line(0, -7, 0, 7);
    return;
  }
  if (id == 102 || id == 20 || id == 21) {
    Label(L"Aa", x - 15, y - 12, 30, 17, b->GetColor(), true);
    if (id == 21) {
      for (int i = 0; i < 3; ++i) {
        b->SetColor(D2D1::ColorF(i == 0   ? 0xd55b65
                                 : i == 1 ? 0x36a77a
                                          : 0x488be0));
        canvas->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(x - 5 + i * 5.f, y + 10), 1.5f, 1.5f),
            b);
      }
    }
    return;
  }
  if (id >= 5 && id <= 7) {
    Label(id == 5   ? L"50"
          : id == 6 ? L"75"
                    : L"100",
          x - 17, y - 9, 34, 13, b->GetColor(), true);
    return;
  }
  auto state = getState();
  Label(page == 1 ? std::to_wstring(state.petSize)
                  : std::to_wstring(state.panelPercent) + L"%",
        x - 18, y - 8, 36, 11, D2D1::ColorF(PetTheme::Muted), true);
}
bool Selected(int id, const ToolbarState &s) {
  return (id == 15 && s.pinned) || (id == 20 && !s.subpixel) ||
         (id == 21 && s.subpixel) ||
         (s.subpixel && ((id == 5 && s.strength < .6f) ||
                         (id == 6 && s.strength > .6f && s.strength < .9f) ||
                         (id == 7 && s.strength > .9f)));
}
std::wstring FeedbackKey(int id) { return L"button:" + std::to_wstring(id); }
std::wstring SelectedKey(int id) { return L"selected:" + std::to_wstring(id); }
void SyncFeedback() {
  const auto items = Items();
  const auto state = getState();
  for (int i = 0; i < (int)items.size(); ++i) {
    const auto id = items[i].id;
    if (!id)
      continue;
    const bool active = Selected(id, state);
    const bool hot = opened && (hovered == i || (keyMode && focus == i));
    const bool down = hot && pressed == i;
    const float opacity = active ? down  ? .32f
                                   : hot ? .21f
                                         : .18f
                          : down ? .23f
                          : hot  ? .09f
                                 : 0;
    SetInteractionFeedback(window, FeedbackKey(id), {hot, down}, opacity);
    SetInteractionFeedback(window, SelectedKey(id), {active, false},
                           active ? 1.f : 0);
  }
}
void Paint() {
  PAINTSTRUCT ps;
  BeginPaint(window, &ps);
  RECT rc;
  GetClientRect(window, &rc);
  if (!canvas)
    UiRenderer::Create(graphics.Get(), window, 96 * scale,
                         D2D1::SizeU(rc.right, rc.bottom), &canvas);
  if (canvas) {
    canvas->BeginDraw();
    PetTheme::PaintSurface(canvas.Get(), rc.right / scale, rc.bottom / scale,
                           SUCCEEDED(materialResult));
    auto items = Items();
    auto state = getState();
    ComPtr<ID2D1SolidColorBrush> brush;
    canvas->CreateSolidColorBrush(D2D1::ColorF(PetTheme::Ink), &brush);
    for (int i = 0; i < (int)items.size(); ++i) {
      auto item = items[i];
      float x = Inset + i * Cell;
      if (item.id)
        PetTheme::PaintFeedback(
            canvas.Get(), D2D1::RectF(x + 2, 5, x + Cell - 2, 41),
            InteractionOpacity(window, FeedbackKey(item.id)), 8);
      const auto amount = InteractionOpacity(window, SelectedKey(item.id));
      const auto ink = D2D1::ColorF(PetTheme::Ink),
                 accent = D2D1::ColorF(PetTheme::Accent);
      brush->SetColor(item.id == 4
                          ? D2D1::ColorF(PetTheme::Error.ink)
                          : D2D1::ColorF(ink.r + (accent.r - ink.r) * amount,
                                         ink.g + (accent.g - ink.g) * amount,
                                         ink.b + (accent.b - ink.b) * amount));
      Icon(item.id, x + Cell / 2, 23, brush.Get());
    }
    int current = hovered >= 0 ? hovered : keyMode ? focus : -1;
    std::wstring hint = current >= 0 && current < (int)items.size()
                            ? items[current].name
                        : page == 1 ? L"宠物大小"
                        : page == 2 ? L"面板大小"
                        : page == 3 ? L"文字显示"
                                    : L"会话与显示设置";
    if (page == 1)
      hint += L" · " + std::to_wstring(state.petSize);
    if (page == 2)
      hint += L" · " + std::to_wstring(state.panelPercent) + L"%";
    Label(hint, 8, 46, Width() - 16, 10, D2D1::ColorF(PetTheme::Muted), true);
    if (canvas->EndDraw() == D2DERR_RECREATE_TARGET)
      canvas.Reset();
  }
  EndPaint(window, &ps);
}
void Fit(bool cursorAnchor) {
  POINT p;
  GetCursorPos(&p);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(cursorAnchor ? MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                    &monitor);
  RECT r;
  GetWindowRect(window, &r);
  int w = (int)(Width() * scale), h = (int)(Height * scale);
  int x = cursorAnchor ? p.x - w / 2 : r.left,
      y = cursorAnchor ? p.y - h - 8 : r.top;
  const auto position = PlaceAvoidingWindows(
      {x, y}, {w, h}, monitor.rcWork,
      getObstacles ? getObstacles() : std::vector<RECT>{},
      FloatingScreenPadding(scale), (LONG)std::ceil(8 * scale));
  destination = position;
  PlacePopup(window, position, {w, h});
  InvalidateRect(window, nullptr, FALSE);
}
void Activate(int index) {
  auto items = Items();
  if (index < 0 || index >= (int)items.size() || !items[index].id)
    return;
  int id = items[index].id;
  if (id == 100 || id == 101 || id == 102 || id == 1000) {
    page = id == 100 ? 1 : id == 101 ? 2 : id == 102 ? 3 : 0;
    hovered = pressed = -1;
    focus = 0;
    ClearInteractionFeedback(window);
    Fit(false);
    SyncFeedback();
    return;
  }
  if (id == 4 || id == 3 || id == 1)
    CloseToolbar();
  execute(id);
  if (opened) {
    Fit(false);
    SyncFeedback();
    // 尺寸操作会重新排列面板/胶囊的置顶顺序；工具栏仍应保持在它们前方。
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(window, nullptr, FALSE);
  }
}
LRESULT CALLBACK Proc(HWND h, UINT message, WPARAM w, LPARAM l) {
  switch (message) {
  case PopupBackendQuery:
    return canvas ? UiRenderer::Backend(h) : -1;
  case WM_NCACTIVATE:
    return DefWindowProcW(h, WM_NCACTIVATE, TRUE, l);
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    Paint();
    return 0;
  case WM_SIZE:
    if (canvas)
      canvas->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
    return 0;
  case WM_DPICHANGED:
    scale = HIWORD(w) / 96.f;
    canvas.Reset();
    if (opened)
      Fit(false);
    return 0;
  case WM_THEMECHANGED:
  case WM_DWMCOMPOSITIONCHANGED:
  case WM_SETTINGCHANGE:
    materialResult = applyMaterial(h);
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  case WM_MOUSEMOVE: {
    int hit = Hit(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale);
    if (hit != hovered || keyMode) {
      hovered = hit;
      keyMode = false;
      SyncFeedback();
      InvalidateRect(h, nullptr, FALSE);
    }
    return 0;
  }
  case WM_LBUTTONDOWN:
    pressed = Hit(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale);
    hovered = pressed;
    keyMode = false;
    SyncFeedback();
    if (pressed < 0)
      CloseToolbar();
    else
      InvalidateRect(h, nullptr, FALSE);
    return 0;
  case WM_LBUTTONUP: {
    int hit = Hit(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale),
        was = pressed;
    pressed = -1;
    SyncFeedback();
    if (hit >= 0 && was == hit)
      Activate(hit);
    else if (hit < 0)
      CloseToolbar();
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  }
  case WM_RBUTTONUP:
    CloseToolbar();
    return 0;
  case WM_KEYDOWN:
    if (w == VK_ESCAPE) {
      CloseToolbar();
      return 0;
    }
    if (w == VK_RETURN || w == VK_SPACE) {
      keyMode = true;
      hovered = -1;
      pressed = focus;
      SyncFeedback();
      Activate(focus);
      return 0;
    }
    if (w == VK_TAB || w == VK_LEFT || w == VK_RIGHT) {
      keyMode = true;
      hovered = -1;
      int direction =
          w == VK_LEFT || (w == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000)) ? -1
                                                                            : 1;
      auto items = Items();
      do {
        focus = (focus + direction + (int)items.size()) % (int)items.size();
      } while (items[focus].id == 0);
      SyncFeedback();
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    break;
  case WM_KEYUP:
    if (w == VK_RETURN || w == VK_SPACE) {
      pressed = -1;
      SyncFeedback();
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    break;
  case WM_ACTIVATE:
    if (LOWORD(w) == WA_INACTIVE && opened)
      CloseToolbar();
    return 0;
  case WM_CAPTURECHANGED:
    if (opened)
      CloseToolbar();
    return 0;
  case WM_CLOSE:
    CloseToolbar();
    return 0;
  }
  return DefWindowProcW(h, message, w, l);
}
} // namespace
void InitToolbar(HWND parent, ID2D1Factory *renderer,
                 IDWriteFactory *typography, ToolbarState (*state)(),
                 void (*action)(int), HRESULT (*material)(HWND),
                 std::vector<RECT> (*obstacles)(), bool preview) {
  owner = parent;
  graphics = renderer;
  text = typography;
  getState = state;
  execute = action;
  applyMaterial = material;
  getObstacles = obstacles;
  WNDCLASSW wc{};
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpfnWndProc = Proc;
  wc.lpszClassName = L"ZCodePet.Toolbar";
  RegisterClassW(&wc);
  window = CreateWindowExW(WS_EX_TOPMOST |
                               (preview ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW),
                           wc.lpszClassName, L"桌宠工具栏", WS_POPUP, 0, 0, 320,
                           64, owner, nullptr, wc.hInstance, nullptr);
  materialResult = applyMaterial(window);
}
void ShowToolbar() {
  if (opened) {
    CloseToolbar();
    return;
  }
  page = 0;
  hovered = pressed = -1;
  focus = 0;
  keyMode = false;
  scale = GetDpiForWindow(owner) / 96.f;
  canvas.Reset();
  Fit(true);
  materialResult = applyMaterial(window);
  opened = true;
  ShowPopupAnimated(window, destination, scale);
  ClearInteractionFeedback(window);
  SyncFeedback();
  SetForegroundWindow(window);
  SetFocus(window);
  SetCapture(window);
  InvalidateRect(window, nullptr, FALSE);
}
void CloseToolbar() {
  if (!opened)
    return;
  opened = false;
  hovered = pressed = -1;
  keyMode = false;
  SyncFeedback();
  if (GetCapture() == window)
    ReleaseCapture();
  HidePopupAnimated(window, scale);
}
bool IsToolbarOpen() { return opened; }
void RefreshToolbarPlacement() {
  if (opened) {
    Fit(false);
    SyncFeedback();
  }
}
void DestroyToolbar() {
  opened = false;
  ClearInteractionFeedback(window);
  if (GetCapture() == window)
    ReleaseCapture();
  HidePopupImmediately(window);
  if (IsWindow(window))
    DestroyWindow(window);
  canvas.Reset();
  text.Reset();
  graphics.Reset();
  window = nullptr;
}
