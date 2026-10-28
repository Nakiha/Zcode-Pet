#include <windows.h>
#include "observer.h"
#include "subpixel.h"
#include "presentation.h"
#include "toolbar.h"
#include "row_layout.h"
#include "status_badge.h"
#include "theme.h"
#include "notice_close.h"
#include "pet_position.h"
#include "floating_layout.h"
#include "popup_motion.h"
#include "ui_renderer.h"
#include "frame_clock.h"
#include "pet_canvas.h"
#include "session_navigation.h"
#include <deque>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <filesystem>
#include <fstream>
#include <gdiplus.h>
#include <memory>
#include <mutex>
#include <shellapi.h>
#include <thread>
#include <windowsx.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
HINSTANCE instance;
HWND pet, panel, capsule;
ComPtr<ID2D1HwndRenderTarget> capsuleTarget;
HRESULT panelMaterialResult = E_PENDING, capsuleMaterialResult = E_PENDING;
HoverPolicy hover;
std::deque<Session> completionQueue;
std::wstring capsuleTitle, capsuleRecap;
std::optional<Session> capsuleSession, panelPressed, noticePressed;
bool capsuleAttention = false;
StateClock stateClock;
StateMotion stateMotion;
ULONGLONG capsuleHoverUntil = 0, celebrateUntil = 0, pausedUntil = 0;
POINT panelDestination{}, capsuleDestination{};
bool demo = false, preview = false, panelPinned = false;
bool navigationPreview = false;
int64_t appStartedAt = 0;
constexpr float PanelWidth = 312, RowHeight = 34, ListTop = 0,
                SessionTextSize = 12;

fs::path base;
ComPtr<ID2D1Factory> factory;
ComPtr<IDWriteFactory> writeFactory;
ComPtr<ID2D1HwndRenderTarget> target;
Snapshot snapshot;
int VisibleRows() { return std::clamp((int)snapshot.sessions.size(), 1, 3); }
float PanelHeight() { return VisibleRows() * RowHeight; }
std::mutex dataMutex;
Snapshot pending;
std::atomic_bool quitting = false;
float dpi = 1, capsuleDpi = 1, petSize = 128, panelZoom = 1.15f;
float CapsuleWidth() { return capsuleRecap.empty() ? 320.f : 420.f; }
float NoticeFontSize() { return SessionTextSize * panelZoom; }
float NoticeLineHeight() { return 17.f * panelZoom; }
float CapsuleHeight() {
  const float lines = capsuleRecap.empty() ? 1.f : 2.f;
  return std::max(20.f, NoticeLineHeight() * lines) + 16.f;
}
fs::path preferencesPath;
int scroll = 0;
bool dragging = false, moved = false;
POINT dragStart{}, petStart{};
bool experimental = false;
float strength = .75f;
std::unique_ptr<Gdiplus::Image> animation;
PetCanvasBuffer petCanvas;
std::vector<UINT> delays;
UINT frame = 0, count = 0;
std::wstring animationName;
void LoadPreferences() {
  wchar_t local[32768];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768))
    return;
  preferencesPath = fs::path(local) / L"ZCodePet/settings.ini";
  panelPinned = GetPrivateProfileIntW(L"UI", L"PinSessions", 0,
                                      preferencesPath.c_str()) != 0;
  petSize =
      (float)std::clamp((int)GetPrivateProfileIntW(L"Size", L"PetDip", 128,
                                                   preferencesPath.c_str()),
                        90, 360);
  panelZoom =
      std::clamp((int)GetPrivateProfileIntW(L"Size", L"PanelPercent", 115,
                                            preferencesPath.c_str()),
                 85, 160) /
      100.f;
}
void SavePreferences() {
  if (preferencesPath.empty())
    return;
  std::error_code ec;
  fs::create_directories(preferencesPath.parent_path(), ec);
  if (ec)
    return;
  WritePrivateProfileStringW(L"UI", L"PinSessions", panelPinned ? L"1" : L"0",
                             preferencesPath.c_str());
  WritePrivateProfileStringW(L"Size", L"PetDip",
                             std::to_wstring((int)petSize).c_str(),
                             preferencesPath.c_str());
  WritePrivateProfileStringW(
      L"Size", L"PanelPercent",
      std::to_wstring((int)std::lround(panelZoom * 100)).c_str(),
      preferencesPath.c_str());
}
void ReflowPopups();
constexpr UINT Refresh = WM_APP + 1, Tray = WM_APP + 2,
               NavigationCompleted = WM_APP + 30, NavigationQuery = WM_APP + 31;

void NavigationFeedback(NavigationCode code) {
  NOTIFYICONDATAW icon{sizeof(icon)};
  icon.hWnd = pet;
  icon.uID = 1;
  icon.uFlags = NIF_TIP;
  wcscpy_s(icon.szTip, code == NavigationCode::Busy
                           ? L"ZCode Pet · 正在定位会话…"
                           : L"ZCode Pet · 只读状态观察");
  if (code != NavigationCode::Busy && code != NavigationCode::Opened) {
    const wchar_t *text = L"未能打开会话，请在 ZCode 中手动选择后重试。";
    if (code == NavigationCode::Ambiguous)
      text = L"项目或会话存在重名，无法唯一定位。请在 ZCode 中手动选择。";
    else if (code == NavigationCode::NotVisible)
      text = L"请先在 ZCode 侧栏展开对应项目，并让会话条目可见后重试。";
    else if (code == NavigationCode::AppClosed ||
             code == NavigationCode::Unavailable)
      text = L"ZCode 未运行或会话信息不可用，暂时无法跳转。";
    else if (code == NavigationCode::Missing)
      text = L"该会话已不在当前观察列表中，请在 ZCode 中查找。";
    else if (code == NavigationCode::FocusFailed)
      text = L"未能聚焦会话条目，请切回 ZCode 后重试。";
    icon.uFlags |= NIF_INFO;
    icon.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcscpy_s(icon.szInfoTitle, L"未能定位 ZCode 会话");
    wcscpy_s(icon.szInfo, text);
  }
  Shell_NotifyIconW(NIM_MODIFY, &icon);
}
void OpenSession(const Session &captured) {
  if (SessionNavigationBusy())
    return;
  auto prepared = PrepareNavigation(captured, snapshot);
  if (demo)
    prepared.code = NavigationCode::Unavailable;
  if (prepared.code != NavigationCode::Idle) {
    NavigationFeedback(prepared.code);
    return;
  }
  if (NavigateToSession(prepared.session))
    NavigationFeedback(NavigationCode::Busy);
}
std::optional<Session> HitSession(HWND window, LPARAM position) {
  if (!snapshot.error.empty() || !PopupTargetVisible(window))
    return {};
  RECT bounds;
  GetClientRect(window, &bounds);
  const int x = GET_X_LPARAM(position), y = GET_Y_LPARAM(position);
  if (x < 0 || x >= bounds.right)
    return {};
  const int row = SessionRowAt(y / dpi, bounds.bottom / dpi, scroll,
                               snapshot.sessions.size());
  return row < 0 ? std::optional<Session>{} : snapshot.sessions[row];
}
// Experimental renderer: a real DirectWrite RGB coverage texture, encoded into
// legal premultiplied BGRA using a reference backdrop (not exact live
// ClearType).
class CoverageRenderer final : public IDWriteTextRenderer {
  ULONG refs = 1;
  ID2D1RenderTarget *rt;
  D2D1_COLOR_F color;
  float scale;

public:
  CoverageRenderer(ID2D1RenderTarget *r, D2D1_COLOR_F c, float s)
      : rt(r), color(c), scale(s) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **p) override {
    if (!p)
      return E_POINTER;
    *p = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) ||
        iid == __uuidof(IDWriteTextRenderer)) {
      *p = static_cast<IDWriteTextRenderer *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto r = --refs;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *v) override {
    *v = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *,
                                                DWRITE_MATRIX *m) override {
    *m = {1, 0, 0, 1, 0, 0};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *v) override {
    *v = scale;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT x, FLOAT y,
                                         DWRITE_MEASURING_MODE mode,
                                         const DWRITE_GLYPH_RUN *run,
                                         const DWRITE_GLYPH_RUN_DESCRIPTION *,
                                         IUnknown *effect) override {
    ComPtr<IDWriteGlyphRunAnalysis> a;
    HRESULT hr = writeFactory->CreateGlyphRunAnalysis(
        run, scale, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, mode, x,
        y, &a);
    if (FAILED(hr))
      return hr;
    RECT b{};
    a->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &b);
    UINT w = b.right - b.left, h = b.bottom - b.top;
    if (!w || !h || w > 16384 || h > 4096)
      return S_OK;
    std::vector<BYTE> cov((size_t)w * h * 3), pixels((size_t)w * h * 4);
    hr = a->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &b, cov.data(),
                               (UINT)cov.size());
    if (FAILED(hr))
      return hr;
    ComPtr<IDWriteRenderingParams> params;
    writeFactory->CreateMonitorRenderingParams(
        MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST), &params);
    bool bgr =
        params && params->GetPixelGeometry() == DWRITE_PIXEL_GEOMETRY_BGR;
    float amount =
        params && params->GetPixelGeometry() == DWRITE_PIXEL_GEOMETRY_FLAT
            ? 0
            : strength;
    // Reference final backdrop after the light translucent card tint.
    const float bg = PetTheme::TextReferenceBackground;
    auto runColor = color;
    ComPtr<ID2D1SolidColorBrush> effectBrush;
    if (effect && SUCCEEDED(effect->QueryInterface(IID_PPV_ARGS(&effectBrush))))
      runColor = effectBrush->GetColor();
    float text[3] = {runColor.r, runColor.g, runColor.b};
    for (size_t i = 0; i < (size_t)w * h; ++i) {
      float c[3] = {cov[i * 3] / 255.f, cov[i * 3 + 1] / 255.f,
                    cov[i * 3 + 2] / 255.f};
      if (bgr)
        std::swap(c[0], c[2]);
      auto encoded = EncodeCoverage({c[0], c[1], c[2]},
                                    {text[0], text[1], text[2]}, bg, amount);
      for (int k = 0; k < 3; ++k)
        pixels[i * 4 + 2 - k] =
            (BYTE)std::clamp(encoded[k] * 255.f, 0.f, encoded[3] * 255.f);
      pixels[i * 4 + 3] = (BYTE)(encoded[3] * 255.f);
    }
    ComPtr<ID2D1Bitmap> bitmap;
    hr = rt->CreateBitmap(
        D2D1::SizeU(w, h), pixels.data(), w * 4,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                 D2D1_ALPHA_MODE_PREMULTIPLIED),
                               96 * scale, 96 * scale),
        &bitmap);
    if (SUCCEEDED(hr))
      rt->DrawBitmap(bitmap.Get(),
                     D2D1::RectF(b.left / scale, b.top / scale, b.right / scale,
                                 b.bottom / scale),
                     1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT,
                                          const DWRITE_UNDERLINE *,
                                          IUnknown *) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT,
                                              const DWRITE_STRIKETHROUGH *,
                                              IUnknown *) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT,
                                             IDWriteInlineObject *, BOOL, BOOL,
                                             IUnknown *) override {
    return S_OK;
  }
};
struct TextLayoutKey {
  std::wstring text;
  float width, height, size;
  bool bold, trailing, centered, tabular;
  bool operator==(const TextLayoutKey &) const = default;
};
struct CachedTextLayout {
  TextLayoutKey key;
  ComPtr<IDWriteTextLayout> layout;
  float offset = 0;
};
CachedTextLayout LayoutText(const TextLayoutKey &key) {
  static std::deque<CachedTextLayout> cache;
  for (const auto &entry : cache)
    if (entry.key == key)
      return entry;
  CachedTextLayout result{key};
  ComPtr<IDWriteTextFormat> fmt;
  writeFactory->CreateTextFormat(
      L"Segoe UI Variable", nullptr,
      key.bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, key.size, L"zh-CN",
      &fmt);
  if (!fmt)
    return result;
  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  if (key.trailing)
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
  if (key.centered)
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  ComPtr<IDWriteInlineObject> ellipsis;
  writeFactory->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis);
  fmt->SetTrimming(&trim, ellipsis.Get());
  writeFactory->CreateTextLayout(key.text.c_str(), (UINT32)key.text.size(),
                                 fmt.Get(), key.width, key.height, &result.layout);
  if (!result.layout)
    return result;
  if (key.tabular) {
    ComPtr<IDWriteTypography> typography;
    writeFactory->CreateTypography(&typography);
    typography->AddFontFeature(
        DWRITE_FONT_FEATURE{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
    result.layout->SetTypography(typography.Get(),
                                  DWRITE_TEXT_RANGE{0, (UINT32)key.text.size()});
  }
  result.offset = CenteredTextOriginY(factory.Get(), result.layout.Get(), key.height);
  cache.push_front(result);
  if (cache.size() > 96)
    cache.pop_back();
  return result;
}
void Text(const std::wstring &s, float x, float y, float w, float h, float size,
          D2D1_COLOR_F color, bool bold = false, bool trailing = false,
          bool centered = false, bool tabular = false, bool shimmer = false) {
  const auto cached = LayoutText({s, w, h, size, bold, trailing, centered, tabular});
  auto layout = cached.layout;
  if (!layout)
    return;
  target->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h),
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  const float originY = y + cached.offset;
  if (experimental) {
    auto r = new CoverageRenderer(target.Get(), color, dpi);
    layout->Draw(nullptr, r, x, originY);
    r->Release();
  } else {
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(color, &brush);
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->DrawTextLayout(D2D1::Point2F(x, originY), layout.Get(), brush.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if (shimmer) {
    // Highlight only the glyphs, with transparent ends so the deep base text
    // stays readable. This also preserves the experimental text's base pass.
    constexpr float band = 56;
    const float phase = (float)(std::fmod(VisualClockMs(), 2800.) / 2800.);
    DWRITE_TEXT_METRICS metrics{};
    const float span = SUCCEEDED(layout->GetMetrics(&metrics))
                           ? std::min(w, metrics.widthIncludingTrailingWhitespace)
                           : w;
    const float left = x - band + (span + band) * phase;
    D2D1_GRADIENT_STOP stops[] = {
        {0, D2D1::ColorF(PetTheme::RunningShine, 0)},
        {.25f, D2D1::ColorF(PetTheme::RunningShine, .12f)},
        {.5f, D2D1::ColorF(PetTheme::RunningShine, 1.f)},
        {.75f, D2D1::ColorF(PetTheme::RunningShine, .12f)},
        {1, D2D1::ColorF(PetTheme::RunningShine, 0)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> light;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, 5, &collection)) &&
        SUCCEEDED(target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(left, y),
                                                 D2D1::Point2F(left + band, y)),
            collection.Get(), &light))) {
      target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
      target->DrawTextLayout(D2D1::Point2F(x, originY), layout.Get(), light.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }
  target->PopAxisAlignedClip();
}
float MeasureTextWidth(const std::wstring &s, float size,
                       bool tabular = false) {
  struct Entry { std::wstring text; float size, width; bool tabular; };
  static std::deque<Entry> cache;
  for (const auto &entry : cache)
    if (entry.text == s && entry.size == size && entry.tabular == tabular)
      return entry.width;
  ComPtr<IDWriteTextFormat> font;
  writeFactory->CreateTextFormat(
      L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
      &font);
  if (!font)
    return size * (float)s.size();
  ComPtr<IDWriteTextLayout> layout;
  writeFactory->CreateTextLayout(s.c_str(), (UINT32)s.size(), font.Get(), 4096,
                                 100, &layout);
  if (!layout)
    return size * (float)s.size();
  if (tabular) {
    ComPtr<IDWriteTypography> typography;
    writeFactory->CreateTypography(&typography);
    typography->AddFontFeature(
        DWRITE_FONT_FEATURE{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
    layout->SetTypography(typography.Get(),
                          DWRITE_TEXT_RANGE{0, (UINT32)s.size()});
  }
  DWRITE_TEXT_METRICS metrics{};
  layout->GetMetrics(&metrics);
  const float width = std::ceil(metrics.widthIncludingTrailingWhitespace);
  cache.push_front({s, size, width, tabular});
  if (cache.size() > 96)
    cache.pop_back();
  return width;
}
PetTheme::StatusTone StateTone(SessionState state) {
  switch (state) {
  case SessionState::Completed:
    return PetTheme::Completed;
  case SessionState::Running:
    return PetTheme::Running;
  case SessionState::Waiting:
    return PetTheme::Waiting;
  case SessionState::Error:
    return PetTheme::Error;
  case SessionState::Interrupted:
    return PetTheme::Interrupted;
  case SessionState::Offline:
    return PetTheme::Offline;
  default:
    return PetTheme::Neutral;
  }
}
int64_t WallClockMs() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
std::wstring Status(const Session &s) {
  const bool known = s.endedAt > 0;
  switch (StateOf(s, snapshot.online)) {
  case SessionState::Offline:
    return L"离线";
  case SessionState::Running:
    return L"工作中";
  case SessionState::Waiting:
    return L"等待回答";
  case SessionState::Completed:
    return known ? L"已完成" : L"上次完成";
  case SessionState::Error:
    return known ? L"错误" : L"上次错误";
  case SessionState::Interrupted:
    return known ? L"已中断" : L"上次中断";
  default:
    return L"状态未知";
  }
}
void DrawIndicator(SessionState state, float x, float y,
                   ID2D1SolidColorBrush *brush) {
  const auto tone = StateTone(state);
  brush->SetColor(D2D1::ColorF(tone.fill));
  if (state == SessionState::Offline) {
    brush->SetColor(D2D1::ColorF(tone.ink));
    target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 6.8f, 6.8f), brush,
                        1.2f);
    return;
  }
  target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 7.5f, 7.5f), brush);
  brush->SetColor(D2D1::ColorF(tone.ink));
  auto line = [&](float x1, float y1, float x2, float y2) {
    target->DrawLine(D2D1::Point2F(x + x1 * 1.6f, y + y1 * 1.6f),
                     D2D1::Point2F(x + x2 * 1.6f, y + y2 * 1.6f), brush,
                     1.5f);
  };
  if (state == SessionState::Completed) {
    line(-2, 0, -.5f, 1.5f);
    line(-.5f, 1.5f, 2, -1.5f);
  } else if (state == SessionState::Error) {
    line(-1.5f, -1.5f, 1.5f, 1.5f);
    line(-1.5f, 1.5f, 1.5f, -1.5f);
  } else if (state == SessionState::Interrupted)
    line(-2, 0, 2, 0);
  else if (state == SessionState::Waiting) {
    line(0, -2, 0, .5f);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + 3.7f), .85f, .85f),
                        brush);
  } else if (state == SessionState::Running) {
    constexpr float Pi = 3.141592654f, radius = 3.8f;
    const float angle = (float)std::fmod(VisualClockMs(), 1200.) * (2 * Pi / 1200);
    auto point = [&](float a) {
      return D2D1::Point2F(x + radius * std::cos(a),
                            y + radius * std::sin(a));
    };
    ComPtr<ID2D1PathGeometry> arc;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(factory->CreatePathGeometry(&arc)) &&
        SUCCEEDED(arc->Open(&sink))) {
      sink->BeginFigure(point(angle), D2D1_FIGURE_BEGIN_HOLLOW);
      sink->AddArc(D2D1::ArcSegment(
          point(angle + Pi * 1.5f), D2D1::SizeF(radius, radius), 0,
          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
      if (SUCCEEDED(sink->Close()))
        target->DrawGeometry(arc.Get(), brush, 1.5f);
    }
  } else if (state == SessionState::Unknown)
    for (float dx : {-2.6f, 0.f, 2.6f})
      target->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(x + dx, y), .7f, .7f), brush);
}
HRESULT ApplySurfaceMaterial(HWND surface) {
  BOOL dark = FALSE;
  DwmSetWindowAttribute(surface, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
  // Native corners and shadows share the same backdrop boundary. No custom
  // region or AccentPolicy: both can leave a detached rectangular material.
  DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
  DwmSetWindowAttribute(surface, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
  DWM_SYSTEMBACKDROP_TYPE material = DWMSBT_TRANSIENTWINDOW;
  HRESULT hr = DwmSetWindowAttribute(surface, DWMWA_SYSTEMBACKDROP_TYPE,
                                     &material, sizeof(material));
  if (surface == panel)
    panelMaterialResult = hr;
  if (surface == capsule)
    capsuleMaterialResult = hr;
  DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
  DwmSetWindowAttribute(surface, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                        sizeof(corners));
  MARGINS glass{-1};
  DwmExtendFrameIntoClientArea(surface, &glass);
  // 悬停浮层不应抢焦点。只保持 non-client 的活跃材质外观，
  // 不调用 SetForegroundWindow/SetFocus 来换取亚克力。
  SendMessageW(surface, WM_NCACTIVATE, TRUE, 0);
  return hr;
}
void PaintPanel() {
  PAINTSTRUCT ps;
  BeginPaint(panel, &ps);
  RECT rc;
  GetClientRect(panel, &rc);
  if (!target) {
    UiRenderer::Create(factory.Get(), panel, 96 * dpi,
                         D2D1::SizeU(rc.right, rc.bottom), &target);
  }
  if (target) {
    target->BeginDraw();
    auto ink = D2D1::ColorF(PetTheme::Ink),
         muted = D2D1::ColorF(PetTheme::Muted);
    float width = rc.right / dpi, height = rc.bottom / dpi;
    const auto animationNow = (uint64_t)VisualClockMs();
    PetTheme::PaintSurface(target.Get(), width, height,
                           SUCCEEDED(panelMaterialResult));
    ComPtr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, .20f), &brush);
    target->PushAxisAlignedClip(D2D1::RectF(0, 0, width, height),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (!snapshot.error.empty())
      Text(snapshot.error, 16, ListTop, width - 32, RowHeight, 12, ink);
    else if (snapshot.sessions.empty())
      Text(L"暂无本地会话", 16, ListTop, width - 32, RowHeight, 12, muted);
    else
      for (size_t i = 0; i < snapshot.sessions.size(); ++i) {
        float y = ListTop + (float)i * RowHeight - scroll;
        if (y + RowHeight < ListTop || y > height)
          continue;
        auto &session = snapshot.sessions[i];
        if (i + 1 < snapshot.sessions.size() && y + RowHeight < height) {
          brush->SetColor(D2D1::ColorF(PetTheme::Divider, .42f));
          target->DrawLine(D2D1::Point2F(RowSidePadding, y + RowHeight - .5f),
                           D2D1::Point2F(width - RowSidePadding,
                                          y + RowHeight - .5f),
                           brush.Get(), .9f);
        }
        const float hop = stateMotion.Offset(session, animationNow);
        DrawIndicator(StateOf(session, snapshot.online), 18, y + 17 + hop,
                      brush.Get());
        const auto state = StateOf(session, snapshot.online);
        auto tone = StateTone(state);
        auto age = stateClock.Age(session, WallClockMs()),
             status = Status(session);
        const auto row = LayoutRow(width, MeasureTextWidth(age, SessionTextSize, true),
                                   MeasureTextWidth(status, 11.5f));
        const auto badge =
            D2D1::RectF(row.badgeLeft, y + 6 + hop, row.badgeRight,
                          y + 28 + hop);
        bool badgeDrawn =
            DrawStatusBadge(target.Get(), factory.Get(), writeFactory.Get(),
                            status, badge, D2D1::ColorF(tone.fill),
                            D2D1::ColorF(tone.ink));
        if (!badgeDrawn) {
          brush->SetColor(D2D1::ColorF(tone.fill));
          target->FillRoundedRectangle(D2D1::RoundedRect(badge, 11, 11),
                                       brush.Get());
        }
        Text(session.title.empty() ? L"未命名会话" : session.title,
             row.titleLeft, y, row.titleRight - row.titleLeft, RowHeight,
             SessionTextSize, ink,
             true, false, false, false, state == SessionState::Running);
        Text(age, row.timeLeft, y, row.timeRight - row.timeLeft, RowHeight,
             SessionTextSize,
             D2D1::ColorF(PetTheme::Time), true, false, false, true);
        if (!badgeDrawn)
          Text(status, row.badgeLeft, y + 6 + hop, row.badgeRight - row.badgeLeft, 22,
               11.5f, D2D1::ColorF(tone.ink), true, false, true);
      }
    target->PopAxisAlignedClip();
    if (target->EndDraw() == D2DERR_RECREATE_TARGET)
      target.Reset();
  }
  EndPaint(panel, &ps);
}
void DrawPet() {
  if (!animation)
    return;
  int side = (int)(petSize * GetDpiForWindow(pet) / 96.f);
  HDC screen = GetDC(nullptr);
  if (!petCanvas.Ensure(screen, side)) {
    ReleaseDC(nullptr, screen);
    return;
  }
  memset(petCanvas.pixels, 0, (size_t)side * side * 4);
  {
    Gdiplus::Graphics g(petCanvas.dc);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    float ratio = std::min((float)side / animation->GetWidth(),
                           (float)side / animation->GetHeight());
    float w = animation->GetWidth() * ratio, h = animation->GetHeight() * ratio;
    g.DrawImage(animation.get(), (side - w) / 2, (side - h) / 2, w, h);
  }
  POINT origin{0, 0};
  SIZE size{side, side};
  KeepRectangularPetHitArea(petCanvas.pixels,(size_t)side*side);
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(pet, screen, nullptr, &size, petCanvas.dc, &origin, 0, &blend,
                      ULW_ALPHA);
  ReleaseDC(nullptr, screen);
  // Mouse movement owns positioning during a drag. An animation frame must
  // never pull the window back against a still-captured pointer.
  if (dragging)
    return;
  RECT bounds;
  GetWindowRect(pet, &bounds);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(pet, MONITOR_DEFAULTTONEAREST), &monitor);
  const auto position = ClampPetToWorkArea({bounds.left, bounds.top},
                                           {side, side}, monitor.rcWork);
  if (position.x != bounds.left || position.y != bounds.top)
    SetWindowPos(pet, nullptr, position.x, position.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}
void SetAnimation(const std::wstring &name) {
  if (name == animationName)
    return;
  auto image = std::make_unique<Gdiplus::Image>(
      (base / L"assets" / (name + L".gif")).c_str());
  if (image->GetLastStatus() != Gdiplus::Ok)
    return;
  animation = std::move(image);
  animationName = name;
  frame = 0;
  count = animation->GetFrameCount(&Gdiplus::FrameDimensionTime);
  delays.assign(std::max(1u, count), 100);
  UINT n = animation->GetPropertyItemSize(PropertyTagFrameDelay);
  if (n) {
    std::vector<BYTE> mem(n);
    auto p = (Gdiplus::PropertyItem *)mem.data();
    if (animation->GetPropertyItem(PropertyTagFrameDelay, n, p) ==
        Gdiplus::Ok) {
      auto v = (UINT *)p->value;
      for (UINT i = 0; i < count && i < p->length / 4; ++i)
        delays[i] = std::clamp(v[i] * 10, 20u, 10000u);
    }
  }
  DrawPet();
  SetTimer(pet, 1, delays[0], nullptr);
}
void DismissPanel() {
  if (panelPinned) {
    panelPinned = false;
    SavePreferences();
  }
  hover.Dismiss();
  HidePopupAnimated(panel, GetDpiForWindow(pet) / 96.f);
}
std::vector<RECT> FloatingObstacles(bool includePanel, bool includeNotice) {
  std::vector<RECT> result;
  for (HWND window : {pet, includePanel ? panel : nullptr,
                      includeNotice ? capsule : nullptr}) {
    RECT bounds;
    if (!window || !IsWindowVisible(window) || !GetWindowRect(window, &bounds))
      continue;
    bounds = PopupAnimationBounds(window);
    if (window == capsule) {
      const auto protrusion = (LONG)std::ceil(12 * capsuleDpi);
      bounds.left -= protrusion;
      bounds.top -= protrusion;
    }
    result.push_back(bounds);
  }
  return result;
}
std::vector<RECT> ToolbarObstacles() { return FloatingObstacles(true, true); }
void PlaceFloating(HWND window, float logicalWidth, float logicalHeight,
                   POINT &destination, int gap) {
  RECT r;
  GetWindowRect(pet, &r);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(pet, MONITOR_DEFAULTTONEAREST), &mi);
  const float scale = window == capsule ? capsuleDpi : dpi;
  const int edge = FloatingScreenPadding(GetDpiForWindow(pet) / 96.f);
  int w = (int)(logicalWidth * scale), h = (int)(logicalHeight * scale);
  w = std::min<int>(w, std::max(1L, mi.rcWork.right - mi.rcWork.left - 2 * edge));
  h = std::min<int>(h, std::max(1L, mi.rcWork.bottom - mi.rcWork.top - 2 * edge));
  int x =
      std::clamp<int>(r.left + (r.right - r.left - w) / 2,
                        mi.rcWork.left + edge, mi.rcWork.right - w - edge);
  int y = r.top - h - gap;
  if (y < mi.rcWork.top + edge)
    y = r.bottom + gap;
  y = std::clamp<int>(y, mi.rcWork.top + edge, mi.rcWork.bottom - h - edge);
  const LONG outset = window == capsule ? (LONG)std::ceil(12 * scale) : 0;
  destination = PlaceAvoidingWindows(
      {x, y}, {w, h}, mi.rcWork, FloatingObstacles(window == capsule, false),
      edge, (LONG)std::ceil(8 * GetDpiForWindow(pet) / 96.f), {outset, outset});
  PlacePopup(window, destination, {w, h});
}
void ReflowPopups() {
  if (!pet)
    return;
  dpi = GetDpiForWindow(pet) / 96.f * panelZoom;
  capsuleDpi = GetDpiForWindow(pet) / 96.f;
  target.Reset();
  capsuleTarget.Reset();
  if (PopupTargetVisible(panel)) {
    PlaceFloating(panel, PanelWidth, PanelHeight(), panelDestination, 6);
    InvalidateRect(panel, nullptr, FALSE);
  }
  if (PopupTargetVisible(capsule)) {
    PlaceFloating(capsule, CapsuleWidth(), CapsuleHeight(), capsuleDestination,
                  IsWindowVisible(panel) ? (int)(PanelHeight() * dpi) + 16 : 8);
    InvalidateRect(capsule, nullptr, FALSE);
  }
  RefreshToolbarPlacement();
}
void MoveAttachedPopups() {
  if (PopupTargetVisible(panel)) {
    PlaceFloating(panel, PanelWidth, PanelHeight(), panelDestination, 6);
    SnapPopupPlacement(panel);
  }
  if (PopupTargetVisible(capsule)) {
    PlaceFloating(capsule, CapsuleWidth(), CapsuleHeight(), capsuleDestination,
                  IsWindowVisible(panel) ? (int)(PanelHeight() * dpi) + 16 : 8);
    SnapPopupPlacement(capsule);
  }
  RefreshToolbarPlacement();
}
void ShowPanel() {
  if (PopupTargetVisible(panel))
    return;
  if (IsWindowVisible(panel)) {
    hover.shown = true;
    ShowPopupAnimated(panel, panelDestination, GetDpiForWindow(pet) / 96.f);
    return;
  }
  dpi = GetDpiForWindow(pet) / 96.f * panelZoom;
  target.Reset();
  PlaceFloating(panel, PanelWidth, PanelHeight(), panelDestination, 6);
  ApplySurfaceMaterial(panel);
  hover.shown = true;
  ShowPopupAnimated(panel, panelDestination, GetDpiForWindow(pet) / 96.f);
  InvalidateRect(panel, nullptr, FALSE);
  if (PopupTargetVisible(capsule))
    PlaceFloating(capsule, CapsuleWidth(), CapsuleHeight(), capsuleDestination,
                  (int)(PanelHeight() * dpi) + 16);
  RefreshToolbarPlacement();
}
void TogglePanel() {
  if (PopupTargetVisible(panel))
    DismissPanel();
  else
    ShowPanel();
}
void UpdateAnimation() {
  const auto now = GetTickCount64();
  bool running = snapshot.online &&
                 std::any_of(snapshot.sessions.begin(), snapshot.sessions.end(),
                             [](const auto &s) { return s.running; });
  SetAnimation(
      SelectPetAnimation(running, now < celebrateUntil, now < pausedUntil,
                         hover.wasInside, dragging,
                         std::any_of(
                             snapshot.sessions.begin(), snapshot.sessions.end(),
                             [](const auto &s) { return s.waiting; })));
}
void ShowNextCompletion() {
  if (completionQueue.empty() || IsWindowVisible(capsule))
    return;
  auto notice = completionQueue.front();
  capsuleSession = notice;
  capsuleAttention = notice.waiting;
  capsuleTitle = notice.title;
  capsuleRecap = notice.waiting
                     ? (notice.questionPreview.empty()
                            ? L"需要你处理，请到 ZCode 查看待确认事项。"
                            : L"等待你回答：" + notice.questionPreview)
                     : notice.recap;
  completionQueue.pop_front();
  dpi = GetDpiForWindow(pet) / 96.f * panelZoom;
  capsuleDpi = GetDpiForWindow(pet) / 96.f;
  capsuleTarget.Reset();
  PlaceFloating(capsule, CapsuleWidth(), CapsuleHeight(), capsuleDestination,
                IsWindowVisible(panel) ? (int)(PanelHeight() * dpi) + 16 : 8);
  ApplySurfaceMaterial(capsule);
  capsuleHoverUntil = 0;
  if (!capsuleAttention)
    celebrateUntil = GetTickCount64() + 4000;
  ShowPopupAnimated(capsule, capsuleDestination, capsuleDpi);
  InvalidateRect(capsule, nullptr, FALSE);
  RefreshToolbarPlacement();
}
void HoverTick() {
  SetFrameClockMonitor(MonitorFromWindow(pet, MONITOR_DEFAULTTONEAREST));
  POINT p;
  GetCursorPos(&p);
  HWND under = WindowFromPoint(p);
  bool inside = under == pet || under == panel || IsChild(panel, under);
  if (IsToolbarOpen())
    inside = true;
  bool show = hover.Update(inside, dragging, GetTickCount64(), panelPinned);
  if (preview && !dragging)
    show = true;
  if (show && !PopupTargetVisible(panel))
    ShowPanel();
  if (!show && PopupTargetVisible(panel))
    HidePopupAnimated(panel, GetDpiForWindow(pet) / 96.f);
  auto now = GetTickCount64();
  ShowNextCompletion();
  const bool noticeVisible = PopupTargetVisible(capsule);
  if (noticeVisible && (under == capsule || IsNoticeClose(under)))
    capsuleHoverUntil = now + 160;
  ShowNoticeClose(capsuleDpi, noticeVisible && now < capsuleHoverUntil);
  UpdateAnimation();
}
void TickPanel() {
  if (!IsWindowVisible(panel)) {
    RequestContinuousFrames(false);
    return;
  }
  // State observation stays at two seconds; visual time and the loading arc
  // follow their own UI cadence and always use elapsed time, never tick counts.
  static std::vector<std::wstring> previousAges;
  static bool wasTransitioning = false;
  std::vector<std::wstring> ages;
  bool loading = false;
  bool transitioning = false;
  const int first = scroll / (int)RowHeight;
  const int end = std::min(first + VisibleRows(), (int)snapshot.sessions.size());
  const auto now = WallClockMs();
  const auto animationNow = (uint64_t)VisualClockMs();
  for (int i = first; i < end; ++i) {
    const auto &session = snapshot.sessions[i];
    ages.push_back(stateClock.Age(session, now));
    loading |= StateOf(session, snapshot.online) == SessionState::Running;
    transitioning |= stateMotion.Active(session, animationNow);
  }
  // Draw one final resting frame after the hop has ended.
  if (loading || transitioning || wasTransitioning || ages != previousAges)
    InvalidateRect(panel, nullptr, FALSE);
  RequestContinuousFrames(loading || transitioning);
  previousAges = std::move(ages);
  wasTransitioning = transitioning;
}
void PaintCapsule() {
  const auto savedDpi = dpi;
  dpi = capsuleDpi;
  auto saved = target;
  target = capsuleTarget;
  PAINTSTRUCT ps;
  BeginPaint(capsule, &ps);
  RECT rc;
  GetClientRect(capsule, &rc);
  if (!target) {
    UiRenderer::Create(factory.Get(), capsule, 96 * dpi,
                         D2D1::SizeU(rc.right, rc.bottom), &target);
  }
  if (target) {
    target->BeginDraw();
    const float width = rc.right / dpi, height = rc.bottom / dpi;
    PetTheme::PaintSurface(target.Get(), width, height,
                           SUCCEEDED(capsuleMaterialResult));
    const float badgeY = height / 2;
    ComPtr<ID2D1SolidColorBrush> green;
    const auto tone = StateTone(capsuleAttention ? SessionState::Waiting
                                                : SessionState::Completed);
    target->CreateSolidColorBrush(
        D2D1::ColorF(tone.fill),
        &green);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(20, badgeY), 10, 10),
                        green.Get());
    green->SetColor(D2D1::ColorF(tone.ink));
    if (capsuleAttention) {
      target->DrawLine(D2D1::Point2F(20, badgeY - 4),
                       D2D1::Point2F(20, badgeY + 1), green.Get(), 2.f);
      target->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(20, badgeY + 4.5f), 1.f, 1.f), green.Get());
    } else {
      target->DrawLine(D2D1::Point2F(15, badgeY),
                       D2D1::Point2F(18.5f, badgeY + 3.3f), green.Get(), 2.f);
      target->DrawLine(D2D1::Point2F(18.5f, badgeY + 3.3f),
                       D2D1::Point2F(25, badgeY - 4), green.Get(), 2.f);
    }
    std::wstring title = capsuleTitle.empty() ? L"未命名会话" : capsuleTitle;
    if (title.size() > 44) {
      title.resize(44);
      if (title.back() >= 0xd800 && title.back() <= 0xdbff)
        title.pop_back();
      title += L"…";
    }
    const auto content = title + (capsuleRecap.empty()
                                     ? capsuleAttention ? L" · 请求回答" : L" · 已完成"
                                     : L" · " + capsuleRecap);
    ComPtr<IDWriteTextFormat> format;
    writeFactory->CreateTextFormat(
        L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, NoticeFontSize(), L"zh-CN",
        &format);
    if (format) {
      format->SetWordWrapping(capsuleRecap.empty()
                                  ? DWRITE_WORD_WRAPPING_NO_WRAP
                                  : DWRITE_WORD_WRAPPING_WRAP);
      format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                              NoticeLineHeight(), 13.5f * panelZoom);
      DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      ComPtr<IDWriteInlineObject> sign;
      writeFactory->CreateEllipsisTrimmingSign(format.Get(), &sign);
      format->SetTrimming(&trim, sign.Get());
      const float textHeight = NoticeLineHeight() * (capsuleRecap.empty() ? 1 : 2);
      ComPtr<IDWriteTextLayout> layout;
      writeFactory->CreateTextLayout(content.c_str(), (UINT32)content.size(),
                                     format.Get(), width - 48, textHeight,
                                     &layout);
      if (layout) {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_TEXT_RANGE{0, (UINT32)title.size()});
        ComPtr<ID2D1SolidColorBrush> body, heading;
        target->CreateSolidColorBrush(D2D1::ColorF(PetTheme::Muted), &body);
        target->CreateSolidColorBrush(D2D1::ColorF(PetTheme::Ink), &heading);
        layout->SetDrawingEffect(heading.Get(),
                                 DWRITE_TEXT_RANGE{0, (UINT32)title.size()});
        const float y = CenteredTextOriginY(factory.Get(), layout.Get(), height);
        const float clipY = (height - textHeight) / 2;
        target->PushAxisAlignedClip(
            D2D1::RectF(36, clipY, width - 12, clipY + textHeight),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (experimental) {
          auto renderer =
              new CoverageRenderer(target.Get(), D2D1::ColorF(PetTheme::Muted), dpi);
          layout->Draw(nullptr, renderer, 36, y);
          renderer->Release();
        } else {
          target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
          target->DrawTextLayout(D2D1::Point2F(36, y), layout.Get(), body.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        target->PopAxisAlignedClip();
      }
    }
    if (target->EndDraw() == D2DERR_RECREATE_TARGET)
      target.Reset();
  }
  EndPaint(capsule, &ps);
  capsuleTarget = target;
  target = saved;
  dpi = savedDpi;
}
LRESULT CALLBACK CapsuleProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
  case PopupBackendQuery:
    return capsuleTarget ? UiRenderer::Backend(h) : -1;
  case WM_NCACTIVATE:
    // Win32 默认非激活处理会把背景亚克力退成实色；保持视觉活跃，不改变焦点。
    return DefWindowProcW(h, WM_NCACTIVATE, TRUE, l);
  case WM_THEMECHANGED:
  case WM_DWMCOMPOSITIONCHANGED:
  case WM_SETTINGCHANGE:
    ApplySurfaceMaterial(h);
    InvalidateRect(h, nullptr, FALSE);
    return 0;

  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    PaintCapsule();
    return 0;
  case WM_SETCURSOR:
    if (LOWORD(l) == HTCLIENT) {
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    }
    break;
  case WM_LBUTTONDOWN:
    noticePressed = capsuleSession;
    SetCapture(h);
    return 0;
  case WM_LBUTTONUP: {
    auto pressed = noticePressed;
    noticePressed.reset();
    if (GetCapture() == h)
      ReleaseCapture();
    RECT bounds;
    GetClientRect(h, &bounds);
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if (pressed && capsuleSession && SameSession(*pressed, *capsuleSession) &&
        PtInRect(&bounds, point) && PopupTargetVisible(h))
      OpenSession(*pressed);
    return 0;
  }
  case WM_CAPTURECHANGED:
    noticePressed.reset();
    return 0;
  case WM_CLOSE:
    ShowNoticeClose(capsuleDpi, false);
    capsuleHoverUntil = 0;
    HidePopupAnimated(h, capsuleDpi);
    return 0;
  case WM_DPICHANGED:
    capsuleDpi = HIWORD(w) / 96.f;
    capsuleTarget.Reset();
    return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}
ToolbarState ToolbarValues() {
  return {panelPinned, experimental, (int)petSize,
          (int)std::lround(panelZoom * 100), strength};
}
void ExecuteToolbarAction(int id) {
  if (id == 1)
    TogglePanel();
  if (id == 15) {
    panelPinned = !panelPinned;
    hover.suppressed = false;
    if (panelPinned)
      ShowPanel();
    SavePreferences();
  }
  if (id == 2 || id == 20 || id == 21) {
    experimental = id == 20 ? false : id == 21 ? true : !experimental;
    InvalidateRect(panel, nullptr, FALSE);
  }
  if (id == 3)
    ShellExecuteW(nullptr, L"open",
                  L"https://github.com/ZanyZebra1127/Little-Remielle", nullptr,
                  nullptr, SW_SHOWNORMAL);
  if (id >= 5 && id <= 7) {
    strength = id == 5 ? .5f : id == 6 ? .75f : 1.f;
    experimental = true;
    InvalidateRect(panel, nullptr, FALSE);
  }
  if (id >= 10 && id <= 17 && id != 15) {
    if (id == 10)
      petSize = std::max(90.f, petSize - 10);
    if (id == 11)
      petSize = std::min(360.f, petSize + 10);
    if (id == 12)
      panelZoom = std::max(.85f, panelZoom - .1f);
    if (id == 13)
      panelZoom = std::min(1.6f, panelZoom + .1f);
    if (id == 16)
      petSize = 128;
    if (id == 17)
      panelZoom = 1.15f;
    if (id == 14) {
      petSize = 128;
      panelZoom = 1.15f;
    }
    DrawPet();
    ReflowPopups();
    SavePreferences();
  }
  if (id == 4)
    DestroyWindow(pet);
}
void Menu() { ShowToolbar(); }
LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
  case PopupBackendQuery:
    return target ? UiRenderer::Backend(h) : -1;
  case WM_TIMER:
    if (w == 3)
      TickPanel();
    return 0;
  case WM_NCACTIVATE:
    // Win32 默认非激活处理会把背景亚克力退成实色；保持视觉活跃，不改变焦点。
    return DefWindowProcW(h, WM_NCACTIVATE, TRUE, l);
  case WM_THEMECHANGED:
  case WM_DWMCOMPOSITIONCHANGED:
  case WM_SETTINGCHANGE:
    ApplySurfaceMaterial(h);
    InvalidateRect(h, nullptr, FALSE);
    return 0;

  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    PaintPanel();
    return 0;
  case WM_SIZE:
    if (target)
      target->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  case WM_DPICHANGED:
    ReflowPopups();
    return 0;
  case WM_KEYDOWN:
    if (w == VK_ESCAPE)
      DismissPanel();
    return 0;
  case WM_CLOSE:
    DismissPanel();
    return 0;
  case WM_MOUSEACTIVATE:
    return MA_ACTIVATE;
  case WM_RBUTTONUP:
    Menu();
    return 0;
  case WM_SETCURSOR:
    if (LOWORD(l) == HTCLIENT && !snapshot.sessions.empty() &&
        snapshot.error.empty()) {
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    }
    break;
  case WM_LBUTTONDOWN:
    panelPressed = HitSession(h, l);
    if (panelPressed)
      SetCapture(h);
    return 0;
  case WM_LBUTTONUP: {
    auto pressed = panelPressed;
    panelPressed.reset();
    const auto released = HitSession(h, l);
    if (GetCapture() == h)
      ReleaseCapture();
    if (pressed && released && SameSession(*pressed, *released))
      OpenSession(*pressed);
    return 0;
  }
  case WM_CAPTURECHANGED:
    panelPressed.reset();
    return 0;
  case WM_MOUSEWHEEL: {
    panelPressed.reset();
    RECT r;
    GetClientRect(h, &r);
    int max = std::max(0, (int)snapshot.sessions.size() * (int)RowHeight -
                              (int)std::lround(r.bottom / dpi));
    scroll = std::clamp(scroll - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA *
                                     (int)RowHeight * 2,
                        0, max);
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  }
  }
  return DefWindowProcW(h, msg, w, l);
}
LRESULT CALLBACK PetProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
  case NavigationCompleted:
    if (const auto result = TakeNavigationResult())
      NavigationFeedback(result->code);
    return 0;
  case NavigationQuery: {
    const auto result = LastNavigationResult();
    if (w >= 4)
      return NavigationDiagnostic((unsigned)w - 4);
    return w == 0   ? (LRESULT)result.code
           : w == 1 ? result.matches
           : w == 2 ? (LRESULT)result.action
                    : SessionNavigationBusy();
  }
  case VisualFrameMessage:
    BeginVisualFrame();
    PulsePopupAnimations();
    TickPanel();
    if (IsWindowVisible(panel))
      UpdateWindow(panel);
    ShowNoticeClose(capsuleDpi, PopupTargetVisible(capsule) &&
                                   GetTickCount64() < capsuleHoverUntil);
    return 0;
  case VisualFrameInfoQuery:
    return w == 0 ? FrameClockDisplayHz()
         : w == 1 ? FrameClockUsesVBlank()
                  : (LRESULT)std::lround(FrameClockAverageMs() * 100);
  case PopupFrameTimeQuery:
    return (LRESULT)std::lround(PopupAverageFrameMs() * 100);
  case WM_TIMER:
    if (w == 2) {
      HoverTick();
      return 0;
    }
    if (animation && count) {
      frame = (frame + 1) % count;
      animation->SelectActiveFrame(&Gdiplus::FrameDimensionTime, frame);
      DrawPet();
      SetTimer(h, 1, delays[frame], nullptr);
    }
    return 0;
  case WM_LBUTTONDOWN: {
    dragging = true;

    moved = false;
    GetCursorPos(&dragStart);
    RECT r;
    GetWindowRect(h, &r);
    petStart = {r.left, r.top};
    SetCapture(h);
    return 0;
  }
  case WM_MOUSEMOVE:
    if (dragging) {
      POINT p;
      GetCursorPos(&p);
      int dx = p.x - dragStart.x, dy = p.y - dragStart.y;
      if (abs(dx) + abs(dy) > 5)
        moved = true;
      if (moved) {
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &monitor)) {
          const int side = (int)(petSize * GetDpiForWindow(h) / 96.f);
          const auto position = ClampPetToWorkArea(
              {petStart.x + dx, petStart.y + dy}, {side, side}, monitor.rcWork);
          RECT current;
          GetWindowRect(h, &current);
          if (position.x != current.left || position.y != current.top) {
            SetWindowPos(h, nullptr, position.x, position.y, 0, 0,
                           SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
            MoveAttachedPopups();
          }
          // Consume cursor movement at the boundary too, so pulling back
          // inward responds immediately instead of retaining an overshoot gap.
          petStart = position;
          dragStart = p;
        }
      }
    }
    return 0;
  case WM_LBUTTONUP:
    if (dragging) {
      dragging = false;
      ReleaseCapture();
      DrawPet();
      MoveAttachedPopups();
      if (!moved)
        ShowPanel();
    }
    return 0;
  case WM_CAPTURECHANGED:
    if (dragging) {
      dragging = false;
      DrawPet();
      MoveAttachedPopups();
    }
    return 0;
  case WM_DPICHANGED:
    DrawPet();
    ReflowPopups();
    SetFrameClockMonitor(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
    return 0;
  case WM_DISPLAYCHANGE:
    DrawPet();
    ReflowPopups();
    SetFrameClockMonitor(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), true);
    return 0;
  case WM_MOUSEWHEEL:
    petSize = std::clamp(
        petSize + GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 15.f, 90.f, 360.f);
    DrawPet();
    ReflowPopups();
    SavePreferences();
    return 0;
  case WM_RBUTTONUP:
    Menu();
    return 0;
  case Tray:
    if (l == WM_LBUTTONUP)
      TogglePanel();
    if (l == WM_RBUTTONUP)
      Menu();
    return 0;
  case Refresh: {
    static CompletionTracker completions(appStartedAt);
    static AttentionTracker attention;
    {
      std::lock_guard lock(dataMutex);
      for (auto &session : attention.Accept(pending)) {
        completionQueue.push_back(session);
      }
      for (auto &session : completions.Accept(pending))
        completionQueue.push_back(session);
      if (snapshot.online && pending.online)
        for (const auto &before : snapshot.sessions)
          if (before.running)
            for (const auto &after : pending.sessions)
              if (after.id == before.id &&
                  after.workspace == before.workspace && !after.running &&
                  after.endedAt > 0 &&
                  (after.result == L"error" || after.result == L"interrupted"))
                pausedUntil = GetTickCount64() + 4000;
      stateMotion.Update(pending, (uint64_t)VisualClockMs(), IsWindowVisible(panel));
      snapshot = std::move(pending);
      stateClock.Update(snapshot, WallClockMs());
      if (navigationPreview && !snapshot.sessions.empty()) {
        // 显式本地验收入口，只生成一条已有会话的预览提醒，不伪造 ZCode 状态。
        auto notice = snapshot.sessions.front();
        notice.waiting = false;
        notice.recap =
            L"导航预览：点击正文打开对应已有会话；关闭钮只关闭提醒。";
        completionQueue.push_back(std::move(notice));
        navigationPreview = false;
      }
      scroll = std::clamp(
          scroll, 0,
          std::max(0, ((int)snapshot.sessions.size() - VisibleRows()) *
                          (int)RowHeight));
    }
    if (IsWindowVisible(panel)) {
      RECT r;
      GetWindowRect(panel, &r);
      int height = (int)(PanelHeight() * dpi);
      if (r.bottom - r.top != height) {
        ReflowPopups();
      }
    }
    ShowNextCompletion();
    UpdateAnimation();
    TickPanel();
    InvalidateRect(panel, nullptr, FALSE);
    return 0;
  }
  case WM_DESTROY:
    quitting = true;
    {
      NOTIFYICONDATAW n{sizeof(n)};
      n.hWnd = h;
      n.uID = 1;
      Shell_NotifyIconW(NIM_DELETE, &n);
    }
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR command, int) {
  instance = inst;
  wchar_t path[32768];
  GetModuleFileNameW(nullptr, path, 32768);
  base = fs::path(path).parent_path();
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (wcsstr(command, L"--navigation-test")) {
    DWORD pixels[] = {0, 0x00010203, 0xff345678, 0x02010101};
    KeepRectangularPetHitArea(pixels, 4);
    return NavigationModelTests() && pixels[0] == 0x01000000 &&
                   pixels[1] == 0x01000000 && pixels[2] == 0xff345678 &&
                   pixels[3] == 0x02010101
               ? 0
               : 1;
  }
  if (wcsstr(command, L"--popup-motion-test"))
    return PopupMotionTests() ? 0 : 1;
  if (wcsstr(command, L"--floating-test"))
    return FloatingLayoutTests() ? 0 : 1;
  if (wcsstr(command, L"--placement-test"))
    return PetPositionTests() ? 0 : 1;
  if (wcsstr(command, L"--badge-test"))
    return StatusBadgeTests() ? 0 : 1;
  if (wcsstr(command, L"--presentation-test"))
    return PresentationTests() ? 0 : 1;
  if (wcsstr(command, L"--self-test"))
    return ObserverTests() && CoverageTests() && PresentationTests() &&
                   RowLayoutTests()
               ? 0
               : 1;
  if (wcsstr(command, L"--recap-probe")) {
    auto data = Observe();
    size_t matched = 0, available = 0;
    for (const auto &row : data.sessions)
      if (row.result == L"completed" && !row.turnId.empty()) {
        ++matched;
        if (!ReadFinalRecap(row).empty())
          ++available;
      }
    std::ofstream f(base / L"recap-probe.txt");
    f << "matched_completed=" << matched << " recap_available=" << available;
    return 0;
  }
  if (wcsstr(command, L"--probe")) {
    auto s = Observe();
    std::ofstream f(base / L"probe.txt");
    f << "windowDpi="
      << GetDpiForWindow(FindWindowW(L"ZCodePet.Native", L"ZCode Pet")) << " ";
    f << "panelVisible="
      << IsWindowVisible(FindWindowW(L"ZCodePet.Sessions", L"ZCode 会话"))
      << " ";
    f << "online=" << s.online << " sessions=" << s.sessions.size()
      << " waiting="
      << std::count_if(s.sessions.begin(), s.sessions.end(),
                       [](const auto &row) { return row.waiting; })
      << " error=" << (!s.error.empty()) << "\n";
    HWND character = FindWindowW(L"ZCodePet.Native", L"ZCode Pet");
    RECT petBounds{};
    MONITORINFO monitor{sizeof(monitor)};
    if (character && GetWindowRect(character, &petBounds) &&
        GetMonitorInfoW(MonitorFromWindow(character, MONITOR_DEFAULTTONEAREST),
                          &monitor)) {
      f << "petBounds=" << petBounds.left << "," << petBounds.top << ","
        << petBounds.right << "," << petBounds.bottom
        << " workArea=" << monitor.rcWork.left << "," << monitor.rcWork.top
        << "," << monitor.rcWork.right << "," << monitor.rcWork.bottom
        << " petInsideWorkArea="
        << (petBounds.left >= monitor.rcWork.left &&
            petBounds.top >= monitor.rcWork.top &&
            petBounds.right <= monitor.rcWork.right &&
            petBounds.bottom <= monitor.rcWork.bottom) << "\n";
      RECT sessionBounds{};
      HWND sessions = FindWindowW(L"ZCodePet.Sessions", L"ZCode 会话");
      if (sessions && IsWindowVisible(sessions) && GetWindowRect(sessions, &sessionBounds))
        f << "panelCenterOffset="
          << ((sessionBounds.left + sessionBounds.right) -
              (petBounds.left + petBounds.right)) / 2. << "\n";
    }
    HWND notice = FindWindowW(L"ZCodePet.Completion", L"任务完成");
    if (notice) {
      BOOL nonClient = FALSE;
      DWM_WINDOW_CORNER_PREFERENCE corners{};
      DWM_SYSTEMBACKDROP_TYPE backdrop{};
      auto ncResult = DwmGetWindowAttribute(notice, DWMWA_NCRENDERING_ENABLED,
                                            &nonClient, sizeof(nonClient));
      auto cornerResult = DwmGetWindowAttribute(
          notice, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
      auto backdropResult = DwmGetWindowAttribute(
          notice, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
      HRGN region = CreateRectRgn(0, 0, 0, 0);
      const int regionType = region ? GetWindowRgn(notice, region) : -1;
      if (region)
        DeleteObject(region);
      f << "noticeVisible=" << IsWindowVisible(notice)
        << " noticeNonClient=" << (SUCCEEDED(ncResult) ? (int)nonClient : -1)
        << " noticeCorners=" << (SUCCEEDED(cornerResult) ? (int)corners : -1)
        << " noticeBackdrop=" << (SUCCEEDED(backdropResult) ? (int)backdrop : -1)
        << " noticeCustomRegion=" << regionType << "\n";
      f << "noticeShadowStyle="
        << ((GetClassLongPtrW(notice, GCL_STYLE) & CS_DROPSHADOW) != 0) << "\n";
    }
    std::vector<RECT> visibleWindows;
    for (HWND window : {character, FindWindowW(L"ZCodePet.Sessions", L"ZCode 会话"),
                        notice, FindWindowW(L"ZCodePet.Toolbar", L"桌宠工具栏")}) {
      RECT bounds;
      if (window && IsWindowVisible(window) && GetWindowRect(window, &bounds))
        visibleWindows.push_back(bounds);
    }
    int overlaps = 0;
    for (size_t i = 0; i < visibleWindows.size(); ++i)
      for (size_t j = i + 1; j < visibleWindows.size(); ++j)
        overlaps += FloatingOverlap(visibleWindows[i], visibleWindows[j]) > 0;
    f << "floatingVisible=" << visibleWindows.size()
      << " floatingOverlaps=" << overlaps << "\n";
    auto backend = [](HWND window) {
      const auto type = SendMessageW(window, PopupBackendQuery, 0, 0);
      return type == D2D1_RENDER_TARGET_TYPE_HARDWARE ? "hardware"
           : type == D2D1_RENDER_TARGET_TYPE_SOFTWARE ? "software" : "unavailable";
    };
    f << "panelRenderer="
      << backend(FindWindowW(L"ZCodePet.Sessions", L"ZCode 会话"))
      << " noticeRenderer=" << backend(notice)
      << " toolbarRenderer=" << backend(FindWindowW(L"ZCodePet.Toolbar", L"桌宠工具栏"))
      << " popupAverageFrameMs="
      << SendMessageW(character, PopupFrameTimeQuery, 0, 0) / 100. << "\n";
    f << "displayHz=" << SendMessageW(character, VisualFrameInfoQuery, 0, 0)
      << " vblankClock=" << SendMessageW(character, VisualFrameInfoQuery, 1, 0)
      << " visualFrameAverageMs="
      << SendMessageW(character, VisualFrameInfoQuery, 2, 0) / 100. << "\n";
    f << "navigationCode=" << SendMessageW(character, NavigationQuery, 0, 0)
      << " navigationMatches=" << SendMessageW(character, NavigationQuery, 1, 0)
      << " navigationAction=" << SendMessageW(character, NavigationQuery, 2, 0)
      << " navigationBusy=" << SendMessageW(character, NavigationQuery, 3, 0)
      << "\n";
    f << "navigationWindows=" << SendMessageW(character, NavigationQuery, 4, 0)
      << " navigationTitles=" << SendMessageW(character, NavigationQuery, 5, 0)
      << " navigationRows=" << SendMessageW(character, NavigationQuery, 6, 0)
      << " navigationProjectNames="
      << SendMessageW(character, NavigationQuery, 7, 0)
      << " navigationProjectType="
      << SendMessageW(character, NavigationQuery, 8, 0) << "\n";
    return s.error.empty() ? 0 : 2;
  }
  if (wcsstr(command, L"--quit")) {
    HWND existing = FindWindowW(L"ZCodePet.Native", L"ZCode Pet");
    if (existing)
      PostMessageW(existing, WM_CLOSE, 0, 0);
    return 0;
  }
  demo = wcsstr(command, L"--demo") != nullptr;
  preview = wcsstr(command, L"--preview") != nullptr;
  navigationPreview =
      preview && !demo && wcsstr(command, L"--navigation-preview");
  appStartedAt = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
  HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\ZCodePet.Native.V1");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(singleton);
    return 0;
  }
  LoadPreferences();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ULONG_PTR gdip;
  Gdiplus::GdiplusStartupInput gi;
  Gdiplus::GdiplusStartup(&gdip, &gi, nullptr);
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               factory.GetAddressOf())) ||
      FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown **>(writeFactory.GetAddressOf()))))
    return 3;
  WNDCLASSW wc{};
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpfnWndProc = PetProc;
  wc.lpszClassName = L"ZCodePet.Native";
  RegisterClassW(&wc);
  wc.lpfnWndProc = CapsuleProc;
  wc.style = CS_DROPSHADOW;
  wc.lpszClassName = L"ZCodePet.Completion";
  RegisterClassW(&wc);
  wc.lpfnWndProc = PanelProc;
  wc.style = 0;
  wc.lpszClassName = L"ZCodePet.Sessions";
  RegisterClassW(&wc);
  RECT work;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const float initialScale = GetDpiForSystem() / 96.f;
  const int initialSide = (int)(petSize * initialScale);
  const int groupWidth = (int)(std::max({PanelWidth * panelZoom, 420.f, petSize}) * initialScale);
  const auto initialPosition = ClampPetToWorkArea(
      {work.right - FloatingScreenPadding(initialScale) - groupWidth / 2 - initialSide / 2,
       work.bottom - initialSide - FloatingScreenPadding(initialScale)},
      {initialSide, initialSide}, work);
  pet = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOPMOST |
          (preview ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE),
      L"ZCodePet.Native", L"ZCode Pet", WS_POPUP, initialPosition.x,
      initialPosition.y, initialSide, initialSide, nullptr, nullptr, inst, nullptr);
  DWORD popupStyle = WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                     (preview ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW);
  panel = CreateWindowExW(popupStyle & ~WS_EX_NOACTIVATE, L"ZCodePet.Sessions",
                          L"ZCode 会话", WS_POPUP, 0, 0, 288, 250, pet, nullptr,
                          inst, nullptr);
  capsule =
      CreateWindowExW(preview ? popupStyle & ~WS_EX_NOACTIVATE : popupStyle,
                      L"ZCodePet.Completion", L"任务完成", WS_POPUP,
                      0, 0, 260, 62, pet, nullptr, inst, nullptr);
  InitNoticeClose(capsule, preview);
  for (HWND surface : {panel, capsule})
    ApplySurfaceMaterial(surface);
  InitToolbar(pet, factory.Get(), writeFactory.Get(), ToolbarValues,
              ExecuteToolbarAction, ApplySurfaceMaterial, ToolbarObstacles, preview);
  NOTIFYICONDATAW n{sizeof(n)};
  n.hWnd = pet;
  n.uID = 1;
  n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  n.uCallbackMessage = Tray;
  n.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(n.szTip, L"ZCode Pet · 只读状态观察");
  Shell_NotifyIconW(NIM_ADD, &n);
  SetAnimation(L"idle");
  if (!animation) {
    MessageBoxW(nullptr, L"缺少 assets/idle.gif，请保持素材目录和 EXE 在一起。",
                L"ZCode Pet", MB_ICONERROR);
    DestroyWindow(pet);
  } else
    SetWindowPos(pet, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetTimer(pet, 2, 40, nullptr);
  SetTimer(panel, 3, 100, nullptr);
  StartFrameClock(pet);
  StartSessionNavigation(pet,NavigationCompleted);
  std::thread worker([] {
    auto demoStart = GetTickCount64();
    std::map<std::wstring, std::wstring> recapCache;
    while (!quitting) {
      Snapshot s;
      try {
        if (demo) {
          s.online = true;
          Session a;
          a.id = L"demo";
          a.workspace = L"demo";
          a.title = L"桌宠状态提示更新";
          a.recap = L"提醒采用原生圆角和系统阴影，文字与会话面板同步；悬停"
                    L"左上角可手动关闭。";
          a.path = L"ZCode Pet";
          a.startedAt = appStartedAt;
          a.running = GetTickCount64() - demoStart < 8000;
          if (!a.running) {
            a.turnId = L"demo-one";
            a.endedAt =
                (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            a.result = L"completed";
          }
          static int64_t demoEnded = 0;
          if (a.endedAt) {
            if (!demoEnded)
              demoEnded = a.endedAt;
            a.endedAt = demoEnded;
          }
          s.sessions = {a};
          // Fixed local fixtures make every semantic palette inspectable by
          // scrolling, without manufacturing events in the observed app.
          Session waiting;
          waiting.id = L"demo-waiting";
          waiting.turnId = L"demo-question";
          waiting.title = L"chat 等待你的回答";
          const auto demoAge = GetTickCount64() - demoStart;
          waiting.waiting = demoAge < 20000;
          waiting.running = !waiting.waiting;
          waiting.resumedAt = appStartedAt + 20000;
          if (demoAge >= 12000)
            waiting.interactionId = L"demo-question-one";
          waiting.questionPreview = L"要继续预览新的宠物配色吗？";
          waiting.waitingSince = appStartedAt - 120000;
          Session completed;
          completed.id = L"demo-completed";
          completed.title = L"Casual Greeting Check-In";
          completed.result = L"completed";
          completed.endedAt = appStartedAt - 3600000;
          Session error;
          error.id = L"demo-error";
          error.title = L"导出失败，请检查文件";
          error.result = L"error";
          error.endedAt = appStartedAt - 240000;
          Session interrupted;
          interrupted.id = L"demo-interrupted";
          interrupted.title = L"已中断的绘图任务";
          interrupted.result = L"interrupted";
          interrupted.endedAt = appStartedAt - 180000;
          Session unknown;
          unknown.id = L"demo-unknown";
          unknown.title = L"尚未获得活动状态";
          unknown.uncertain = true;
          s.sessions.insert(s.sessions.end(),
                            {waiting, completed, error, interrupted, unknown});
        } else {
          s = Observe();
          for (auto &session : s.sessions)
            if (session.result == L"completed" &&
                session.endedAt > appStartedAt && !session.turnId.empty()) {
              auto key = session.workspace + L"\n" + session.id + L"\n" +
                         session.turnId;
              auto i = recapCache.find(key);
              if (i == recapCache.end())
                i = recapCache.emplace(key, ReadFinalRecap(session)).first;
              session.recap = i->second;
            }
        }
      } catch (...) {
        s.error = L"状态读取失败";
      }
      {
        std::lock_guard lock(dataMutex);
        pending = std::move(s);
      }
      PostMessageW(pet, Refresh, 0, 0);
      for (int i = 0; i < 20 && !quitting; ++i)
        Sleep(100);
    }
  });
  if (panelPinned || wcsstr(command, L"--panel"))
    TogglePanel();
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  quitting = true;
  worker.join();
  StopSessionNavigation();
  StopFrameClock();
  DestroyToolbar();
  ClearPopupAnimations();
  DestroyNoticeClose();
  DestroyWindow(capsule);
  capsuleTarget.Reset();
  DestroyWindow(panel);
  animation.reset();
  petCanvas.Reset();
  target.Reset();
  writeFactory.Reset();
  factory.Reset();
  Gdiplus::GdiplusShutdown(gdip);
  CoUninitialize();
  CloseHandle(singleton);
  return 0;
}
