#include "data_sources.h"
#include "embedded_assets.h"
#include "text_raster.h"
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
#include "interaction_motion.h"
#include "notice_model.h"
#include "panel_scene.h"
#include "text_mask.h"
#include "panel_composition.h"
#include "resource.h"
#include "spine_pet.h"
#include "software_raster.h"
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
HWND pet, panel;
HICON trayIcon=nullptr;
bool trayRegistered=false,trayVersion4=false,trayIconOwned=false;
UINT taskbarCreatedMessage=RegisterWindowMessageW(L"TaskbarCreated");
HRESULT panelMaterialResult = E_PENDING;
HRESULT panelDrawResult = S_OK;
uint64_t panelDrawErrors = 0;
struct NoticeSurface {
  HWND window = nullptr;
  NoticeRecord record;
  ComPtr<ID2D1HwndRenderTarget> target;
  HRESULT material = E_PENDING;
  float scale = 1;
  POINT destination{};
  ULONGLONG hoverUntil = 0;
  std::optional<Session> pressed;
  bool closing = false;
};
std::vector<std::unique_ptr<NoticeSurface>> notices;
NoticeSurface *FindNotice(HWND window) {
  for (auto &notice : notices)
    if (notice->window == window)
      return notice.get();
  return nullptr;
}
std::wstring NoticeCaption(const NoticeRecord &record) {
  return (record.Attention() ? L"等待回答 · " : L"已完成 · ") + record.Title();
}
HoverPolicy hover;
std::deque<Session> incomingNotices;
std::optional<Session> panelPressed;
std::wstring panelHotKey;
StateClock stateClock;
StateMotion stateMotion;
ULONGLONG pausedUntil = 0;
CompletionCelebrations celebrations;
POINT panelDestination{};
bool demo = false, preview = false, panelPinned = false;
bool navigationPreview = false;
BackdropMode backdropMode = BackdropMode::Acrylic;
std::wstring gpuProfileScene;
bool gpuProfileNoAcrylic=false,gpuProfileNoShimmer=false,gpuProfileNoSpinner=false,
     gpuProfileFrozenPet=false,gpuProfilePetOnly=false;
bool gpuProfileNoCache = false;
bool gpuProfileNoMask = false;
bool gpuProfileLegacy = false;
PanelComposition panelComposition;
SpinePet spinePet;
bool spineHardware=false;
double lastSpineFrame=0;
uint64_t panelDraws=0,noticeDraws=0,petDraws=0,visualFrames=0;
uint64_t panelCacheBuilds=0,panelCacheHits=0,panelCacheFallbacks=0;
int64_t appStartedAt = 0;
constexpr float PanelWidth = 312, RowHeight = 34, ListTop = 0,
                SessionTextSize = 12;

fs::path base;
ComPtr<ID2D1Factory> factory;
ComPtr<IDWriteFactory> writeFactory;
ComPtr<ID2D1HwndRenderTarget> target;
struct PanelTextureCache {
  ComPtr<ID2D1RenderTarget> owner;
  ComPtr<ID2D1BitmapRenderTarget> surface;
  ComPtr<ID2D1Bitmap> bitmap;
  std::optional<PanelScene> scene;
  void Reset() { bitmap.Reset(); surface.Reset(); owner.Reset(); scene.reset(); }
} panelTexture;
std::optional<PanelScene> lastPanelScene;
Snapshot snapshot;
int sessionRows = DefaultSessionRows;
int VisibleRows();
float PanelHeight() { return VisibleRows() * RowHeight; }
std::mutex dataMutex;
Snapshot pending;
std::atomic_bool quitting = false;
float dpi = 1, petSize = 128, panelZoom = 1.15f;
int VisibleRows() {
  int capacity = 10;
  if (pet) {
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(pet, MONITOR_DEFAULTTONEAREST), &monitor)) {
      const float scale = GetDpiForWindow(pet) / 96.f;
      const int available = monitor.rcWork.bottom - monitor.rcWork.top -
                            2 * FloatingScreenPadding(scale);
      capacity = (int)std::floor(available / (RowHeight * scale * panelZoom));
    }
  }
  return VisibleSessionRows((int)snapshot.sessions.size(), sessionRows, capacity);
}
float NoticeFontSize() { return SessionTextSize; }
float NoticeLineHeight() { return 17.f; }
float NoticeHeight(const NoticeSurface &notice) {
  return RowHeight + (notice.record.Attention() ? NoticeLineHeight() : 0);
}
fs::path preferencesPath;
int scroll = 0;
bool dragging = false, moved = false;
POINT dragStart{}, petStart{};
bool experimental = false;
float strength = .75f;
std::wstring animationName;
void LoadPreferences() {
  preferencesPath = PetSettingsFile();
  if (preferencesPath.empty())
    return;
  panelPinned = GetPrivateProfileIntW(L"UI", L"PinSessions", 0,
                                      preferencesPath.c_str()) != 0;
  backdropMode = ReadBackdropMode((int)GetPrivateProfileIntW(
      L"UI", L"Backdrop", 0, preferencesPath.c_str()));
  sessionRows = ClampSessionRows((int)GetPrivateProfileIntW(
      L"UI", L"SessionRows", DefaultSessionRows, preferencesPath.c_str()));
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
  if (preferencesPath.empty() || !gpuProfileScene.empty())
    return;
  std::error_code ec;
  fs::create_directories(preferencesPath.parent_path(), ec);
  if (ec)
    return;
  WritePrivateProfileStringW(L"UI", L"PinSessions", panelPinned ? L"1" : L"0",
                             preferencesPath.c_str());
  WritePrivateProfileStringW(L"UI", L"Backdrop",
      std::to_wstring((int)backdropMode).c_str(), preferencesPath.c_str());
  WritePrivateProfileStringW(L"UI", L"Spine", nullptr, preferencesPath.c_str());
  WritePrivateProfileStringW(L"UI", L"SessionRows", std::to_wstring(sessionRows).c_str(),
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
LRESULT CALLBACK CapsuleProc(HWND,UINT,WPARAM,LPARAM);
constexpr UINT Refresh = WM_APP + 1, Tray = WM_APP + 2,
               NavigationCompleted = WM_APP + 30, NavigationQuery = WM_APP + 31,
               NoticeInfoQuery=WM_APP+32,GpuRenderQuery=WM_APP+34;
constexpr UINT CompositionQuery=WM_APP+36;
constexpr UINT TrayInfoQuery=WM_APP+38;
constexpr UINT SpineInfoQuery=WM_APP+40;
constexpr UINT SessionRowsQuery=WM_APP+42;
bool InitializePetRenderer(bool hardware);
void UpdateTrayIcon(bool reload=false) {
  HICON previous=trayIcon;
  const bool previousOwned=trayIconOwned;
  if (!trayIcon || reload) {
    const auto trayDpi=GetDpiForWindow(pet);
    trayIcon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(IDI_ZCODEPET),IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON,trayDpi),GetSystemMetricsForDpi(SM_CYSMICON,trayDpi),0);
    trayIconOwned=trayIcon!=nullptr;
    if (!trayIcon) trayIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_ZCODEPET));
  }
  NOTIFYICONDATAW icon{sizeof(icon)};
  icon.hWnd=pet;icon.uID=1;
  icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
  icon.uCallbackMessage=Tray;icon.hIcon=trayIcon;
  wcscpy_s(icon.szTip,L"ZCode Pet · 只读状态观察");
  bool added=!trayRegistered;
  trayRegistered=Shell_NotifyIconW(added ? NIM_ADD : NIM_MODIFY,&icon)!=FALSE;
  if (!trayRegistered && !added) {
    trayRegistered=Shell_NotifyIconW(NIM_ADD,&icon)!=FALSE;
    added=true;
  }
  if (trayRegistered && added) {
    icon.uVersion=NOTIFYICON_VERSION_4;
    trayVersion4=Shell_NotifyIconW(NIM_SETVERSION,&icon)!=FALSE;
  }
  if (previous && previous!=trayIcon && previousOwned) DestroyIcon(previous);
}

void NavigationFeedback(NavigationCode code) {
  NOTIFYICONDATAW icon{sizeof(icon)};
  icon.hWnd = pet;
  icon.uID = 1;
  icon.uFlags = NIF_TIP;
  wcscpy_s(icon.szTip, code == NavigationCode::Busy
                           ? L"ZCode Pet · 正在定位会话…"
                           : L"ZCode Pet · 只读状态观察");
  if (code != NavigationCode::Busy && code != NavigationCode::Opened) {
    const wchar_t *text = L"未能打开会话，请在对应应用中手动选择后重试。";
    if (code == NavigationCode::Ambiguous)
      text = L"项目或会话存在重名，无法唯一定位。请在对应应用中手动选择。";
    else if (code == NavigationCode::NotFound)
      text = L"找不到条目：请在应用侧栏展开对应工作区或取消筛选后重试。";
    else if (code == NavigationCode::Offscreen)
      text = L"条目离屏：请滚动应用侧栏，让该会话进入可见区域后重试。";
    else if (code == NavigationCode::AppClosed ||
             code == NavigationCode::Unavailable)
      text = L"对应应用未运行或会话信息不可用，暂时无法跳转。";
    else if(code==NavigationCode::UnsupportedSource)
      text=L"Harness 会话监控已接入，桌面跳转尚未适配，请在 Harness 中选择会话。";
    else if (code == NavigationCode::Missing)
      text = L"该会话已不在当前观察列表中，请在对应应用中查找。";
    else if (code == NavigationCode::FocusFailed)
      text = L"未能聚焦会话条目，请切回对应应用后重试。";
    icon.uFlags |= NIF_INFO;
    icon.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcscpy_s(icon.szInfoTitle, L"未能定位会话");
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
  if (NavigateToSession(prepared.session, snapshot.sessions))
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
std::wstring FeedbackKey(const Session &session) {
  return session.workspace + L"\n" + session.id;
}
void UpdatePanelFeedback() {
  if (!gpuProfileScene.empty()) return;
  POINT point;
  GetCursorPos(&point);
  const auto under = WindowFromPoint(point);
  std::optional<Session> hit;
  if (under == panel && PopupTargetVisible(panel)) {
    ScreenToClient(panel, &point);
    hit = HitSession(panel, MAKELPARAM(point.x, point.y));
  }
  const auto next = hit ? FeedbackKey(*hit) : L"";
  if (!panelHotKey.empty() && panelHotKey != next)
    SetInteractionFeedback(panel, panelHotKey, {}, 0);
  panelHotKey = next;
  if (hit) {
    const bool down = panelPressed && SameSession(*panelPressed, *hit);
    SetInteractionFeedback(panel, next, {true, down}, down ? .30f : .16f);
  }
}
void UpdateNoticeFeedback(HWND window) {
  auto notice = FindNotice(window);
  if (!notice)
    return;
  POINT point;
  GetCursorPos(&point);
  const bool hot = !notice->closing && PopupTargetVisible(window) &&
                   WindowFromPoint(point) == window;
  const bool down = hot && notice->pressed &&
                    SameSession(*notice->pressed, notice->record.event);
  SetInteractionFeedback(window, L"body", {hot, down},
                         down  ? .28f
                         : hot ? .14f
                               : 0);
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
          bool centered = false, bool tabular = false, bool shimmer = false,
          ID2D1RenderTarget *canvas = nullptr, bool drawBase = true) {
  if (!canvas)
    canvas = target.Get();
  const auto cached = LayoutText({s, w, h, size, bold, trailing, centered, tabular});
  auto layout = cached.layout;
  if (!layout)
    return;
  canvas->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h),
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  const float originY = y + cached.offset;
  if (drawBase && experimental) {
    auto r = new CoverageRenderer(canvas, color, dpi);
    layout->Draw(nullptr, r, x, originY);
    r->Release();
  } else if (drawBase) {
    ComPtr<ID2D1SolidColorBrush> brush;
    canvas->CreateSolidColorBrush(color, &brush);
    canvas->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    canvas->DrawTextLayout(TextRasterOrigin(canvas, x, originY), layout.Get(), brush.Get(),
                           StableTextOptions);
  }
  if (shimmer && !gpuProfileNoShimmer) {
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
    if (SUCCEEDED(canvas->CreateGradientStopCollection(stops, 5, &collection)) &&
        SUCCEEDED(canvas->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(left, y),
                                                 D2D1::Point2F(left + band, y)),
            collection.Get(), &light))) {
      canvas->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
      if (gpuProfileNoMask || !PaintTextMask(canvas,layout.Get(),light.Get(),x,originY,
                                            D2D1::RectF(x,y,x+w,y+h),dpi))
        canvas->DrawTextLayout(TextRasterOrigin(canvas, x, originY), layout.Get(), light.Get(),
                               StableTextOptions);
    }
  }
  canvas->PopAxisAlignedClip();
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
  if(s.catchingUp)return L"同步中";
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
                   ID2D1SolidColorBrush *brush, ID2D1RenderTarget *canvas = nullptr) {
  if (!canvas) canvas = target.Get();
  ComPtr<ID2D1Factory> graphics;
  canvas->GetFactory(&graphics);
  const auto tone = StateTone(state);
  brush->SetColor(D2D1::ColorF(tone.fill));
  if (state == SessionState::Offline) {
    brush->SetColor(D2D1::ColorF(tone.ink));
    canvas->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 6.8f, 6.8f), brush,
                        1.2f);
    return;
  }
  canvas->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 7.5f, 7.5f), brush);
  brush->SetColor(D2D1::ColorF(tone.ink));
  const auto rounded=RoundStatusStroke(graphics.Get());
  auto line = [&](float x1, float y1, float x2, float y2) {
    canvas->DrawLine(D2D1::Point2F(x + x1 * 1.6f, y + y1 * 1.6f),
                     D2D1::Point2F(x + x2 * 1.6f, y + y2 * 1.6f), brush,
                     1.5f,rounded);
  };
  if (state == SessionState::Completed) {
    DrawCheckMark(canvas,graphics.Get(),brush,x,y,1.6f,1.5f);
  } else if (state == SessionState::Error) {
    line(-1.5f, -1.5f, 1.5f, 1.5f);
    line(-1.5f, 1.5f, 1.5f, -1.5f);
  } else if (state == SessionState::Interrupted)
    line(-2, 0, 2, 0);
  else if (state == SessionState::Waiting) {
    canvas->DrawLine(D2D1::Point2F(x,y-3.2f),D2D1::Point2F(x,y+.8f),brush,1.8f,rounded);
    canvas->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + 3.7f), .95f, .95f),
                        brush);
  } else if (state == SessionState::Running) {
    if (gpuProfileNoSpinner) return;
    constexpr float Pi = 3.141592654f, radius = 3.8f;
    const float angle = (float)std::fmod(VisualClockMs(), 1200.) * (2 * Pi / 1200);
    auto point = [&](float a) {
      return D2D1::Point2F(x + radius * std::cos(a),
                            y + radius * std::sin(a));
    };
    ComPtr<ID2D1PathGeometry> arc;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(graphics->CreatePathGeometry(&arc)) &&
        SUCCEEDED(arc->Open(&sink))) {
      sink->BeginFigure(point(angle), D2D1_FIGURE_BEGIN_HOLLOW);
      sink->AddArc(D2D1::ArcSegment(
          point(angle + Pi * 1.5f), D2D1::SizeF(radius, radius), 0,
          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
      sink->EndFigure(D2D1_FIGURE_END_OPEN);
      if (SUCCEEDED(sink->Close()))
        canvas->DrawGeometry(arc.Get(), brush, 1.8f,rounded);
    }
  } else if (state == SessionState::Unknown)
    for (float dx : {-2.6f, 0.f, 2.6f})
      canvas->FillEllipse(
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
  DWM_SYSTEMBACKDROP_TYPE material = gpuProfileNoAcrylic ? DWMSBT_NONE : BackdropType(backdropMode);
  HRESULT hr = DwmSetWindowAttribute(surface, DWMWA_SYSTEMBACKDROP_TYPE,
                                     &material, sizeof(material));
  if (material == DWMSBT_NONE) hr=E_FAIL;
  if (surface == panel)
    panelMaterialResult = hr;
  if (auto notice=FindNotice(surface))
    notice->material=hr;
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
PanelScene CapturePanelScene(float width, float height) {
  PanelScene scene{width,       height,       dpi,
                   strength,    experimental, SUCCEEDED(panelMaterialResult),
                   backdropMode};
  scene.message = snapshot.error.empty() && snapshot.sessions.empty()
                      ? L"暂无本地会话"
                      : snapshot.error;
  if (!scene.message.empty())
    return scene;
  const auto now = WallClockMs();
  const auto animationNow = (uint64_t)VisualClockMs();
  for (size_t i = 0; i < snapshot.sessions.size(); ++i) {
    const float y = ListTop + (float)i * RowHeight - scroll;
    if (y + RowHeight < ListTop || y > height)
      continue;
    const auto &session = snapshot.sessions[i];
    scene.rows.push_back(
        {(session.source==SourceId::DeepSeek?L"DS · ":L"")+ (session.title.empty() ? std::wstring(L"未命名会话") : session.title),
         stateClock.Age(session, now), Status(session),
         StateOf(session, snapshot.online), y,
         stateMotion.Offset(session, animationNow),
         InteractionOpacity(panel, FeedbackKey(session)),
         i + 1 < snapshot.sessions.size() && y + RowHeight < height});
    scene.rows.back().identity=FeedbackKey(session);
  }
  return scene;
}
void DrawPanelScene(ID2D1RenderTarget *canvas, const PanelScene &scene,
                    bool dynamic) {
  const auto ink = D2D1::ColorF(PetTheme::Ink);
  ComPtr<ID2D1Factory> graphics;
  canvas->GetFactory(&graphics);
  if (!dynamic)
    PetTheme::PaintSurface(canvas, scene.width, scene.height, scene.material,
                           8.f, false);
  ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(canvas->CreateSolidColorBrush(ink, &brush)))
    return;
  canvas->PushAxisAlignedClip(D2D1::RectF(0, 0, scene.width, scene.height),
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  if (!dynamic && !scene.message.empty())
    Text(scene.message, 16, ListTop, scene.width - 32, RowHeight, 12, ink,
         false, false, false, false, false, canvas);
  for (const auto &item : scene.rows) {
    const auto row = LayoutRow(
        scene.width, MeasureTextWidth(item.age, SessionTextSize, true),
        MeasureTextWidth(item.status, 11.5f));
    const auto y = item.y, hop = item.hop;
    if (dynamic) {
      if (item.state != SessionState::Running)
        continue;
      DrawIndicator(item.state, RowIndicatorCenter, y + 17 + hop, brush.Get(),
                    canvas);
      Text(item.title, row.titleLeft, y, row.titleRight - row.titleLeft,
           RowHeight, SessionTextSize, ink, true, false, false, false, true,
           canvas, false);
      continue;
    }
    PetTheme::PaintFeedback(canvas,
                            D2D1::RectF(0, y, scene.width, y + RowHeight),
                            item.feedback, 0);
    if (item.divider) {
      brush->SetColor(D2D1::ColorF(PetTheme::Divider, .42f));
      canvas->DrawLine(
          D2D1::Point2F(RowSidePadding, y + RowHeight - .5f),
          D2D1::Point2F(scene.width - RowSidePadding, y + RowHeight - .5f),
          brush.Get(), .9f);
    }
    if (item.state != SessionState::Running)
      DrawIndicator(item.state, RowIndicatorCenter, y + 17 + hop, brush.Get(),
                    canvas);
    else {
      brush->SetColor(D2D1::ColorF(PetTheme::Running.fill));
      canvas->FillEllipse(D2D1::Ellipse(D2D1::Point2F(RowIndicatorCenter,y+17+hop),7.5f,7.5f),brush.Get());
    }
    const auto tone = StateTone(item.state);
    const auto badge =
        D2D1::RectF(row.badgeLeft, y + 6 + hop, row.badgeRight, y + 28 + hop);
    const bool badgeDrawn =
        DrawStatusBadge(canvas, graphics.Get(), writeFactory.Get(), item.status,
                        badge, D2D1::ColorF(tone.fill), D2D1::ColorF(tone.ink));
    if (!badgeDrawn) {
      brush->SetColor(D2D1::ColorF(tone.fill));
      canvas->FillRoundedRectangle(D2D1::RoundedRect(badge, 11, 11),
                                   brush.Get());
      Text(item.status, row.badgeLeft, y + 6 + hop,
           row.badgeRight - row.badgeLeft, 22, 11.5f, D2D1::ColorF(tone.ink),
           true, false, true, false, false, canvas);
    }
    Text(item.title, row.titleLeft, y, row.titleRight - row.titleLeft,
         RowHeight, SessionTextSize, ink, true, false, false, false, false,
         canvas);
    Text(item.age, row.timeLeft, y, row.timeRight - row.timeLeft, RowHeight,
         SessionTextSize, D2D1::ColorF(PetTheme::Time), true, false, false,
         true, false, canvas);
  }
  canvas->PopAxisAlignedClip();
  if (!dynamic)
    PetTheme::PaintSurfaceBorder(canvas, scene.width, scene.height);
}
bool PreparePanelTexture(const PanelScene &scene, D2D1_SIZE_U pixels) {
  if (gpuProfileNoCache)
    return false;
  const bool compatible =
      panelTexture.owner.Get() == target.Get() && panelTexture.surface &&
      panelTexture.surface->GetPixelSize().width == pixels.width &&
      panelTexture.surface->GetPixelSize().height == pixels.height &&
      panelTexture.scene && panelTexture.scene->dpi == scene.dpi;
  if (!compatible) {
    panelTexture.Reset();
    const auto size = D2D1::SizeF(scene.width, scene.height);
    if (FAILED(target->CreateCompatibleRenderTarget(
            &size, &pixels, nullptr, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
            &panelTexture.surface)))
      return false;
    panelTexture.owner = target;
  }
  if (panelTexture.scene && *panelTexture.scene == scene &&
      panelTexture.bitmap) {
    ++panelCacheHits;
    return true;
  }
  panelTexture.bitmap.Reset();
  panelTexture.surface->BeginDraw();
  DrawPanelScene(panelTexture.surface.Get(), scene, false);
  if (FAILED(panelTexture.surface->EndDraw()) ||
      FAILED(panelTexture.surface->GetBitmap(&panelTexture.bitmap))) {
    panelTexture.Reset();
    return false;
  }
  panelTexture.scene = scene;
  ++panelCacheBuilds;
  return true;
}
bool UpdatePanelComposition(const PanelScene &scene) {
  if (gpuProfileLegacy || !panelComposition.Ensure(panel)) return false;
  std::vector<ComposedSession> rows;
  for (const auto &item:scene.rows) {
    if (item.state!=SessionState::Running) continue;
    const auto row=LayoutRow(scene.width,MeasureTextWidth(item.age,SessionTextSize,true),
                             MeasureTextWidth(item.status,11.5f));
    const float width=row.titleRight-row.titleLeft;
    auto layout=LayoutText({item.title,width,RowHeight,SessionTextSize,true,false,false,false});
    DWRITE_TEXT_METRICS metrics{};
    const float span=layout.layout && SUCCEEDED(layout.layout->GetMetrics(&metrics))
        ? std::min(width,metrics.widthIncludingTrailingWhitespace) : width;
    rows.push_back({item.identity,item.title,layout.layout,row.titleLeft,item.y,width,
                    RowHeight,layout.offset,span,item.y+17+item.hop});
  }
  return SUCCEEDED(panelComposition.Update(rows,scene.width,scene.height,scene.dpi,
                                            !gpuProfileNoShimmer,!gpuProfileNoSpinner));
}
void PaintPanel() {
  ++panelDraws;
  PAINTSTRUCT ps;
  BeginPaint(panel, &ps);
  RECT rc;
  GetClientRect(panel, &rc);
  const auto pixels = D2D1::SizeU(rc.right, rc.bottom);
  const auto scene=CapturePanelScene(rc.right/dpi,rc.bottom/dpi);
  if (UpdatePanelComposition(scene)) {
    target.Reset();panelTexture.Reset();
    if (!lastPanelScene || *lastPanelScene!=scene)
      panelDrawResult=panelComposition.PaintBase(scene.width,scene.height,scene.dpi,
          [&](ID2D1RenderTarget *canvas) { DrawPanelScene(canvas,scene,false); });
    if (SUCCEEDED(panelDrawResult)) {
      lastPanelScene=scene;
      EndPaint(panel,&ps);
      return;
    }
    ++panelDrawErrors;
  }
  if (!target) {
    UiRenderer::Create(factory.Get(), panel, 96 * dpi, pixels, &target);
  }
  if (target) {
    const bool cached = PreparePanelTexture(scene, pixels);
    target->BeginDraw();
    if (cached) {
      target->Clear(D2D1::ColorF(0, 0.f));
      target->DrawBitmap(panelTexture.bitmap.Get(),
                         D2D1::RectF(0, 0, scene.width, scene.height), 1,
                         D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    } else {
      ++panelCacheFallbacks;
      DrawPanelScene(target.Get(), scene, false);
    }
    DrawPanelScene(target.Get(), scene, true);
    panelDrawResult = target->EndDraw();
    if (SUCCEEDED(panelDrawResult)) lastPanelScene=scene;
    if (FAILED(panelDrawResult))
      ++panelDrawErrors;
    if (panelDrawResult == D2DERR_RECREATE_TARGET) {
      panelTexture.Reset();
      target.Reset();
    }
  }
  EndPaint(panel, &ps);
}
void ClampPetPosition() {
  const int side=(int)(petSize*GetDpiForWindow(pet)/96.f);
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
bool DrawPet() {
  ++petDraws;
  if (!spinePet.Ready() ||
      !spinePet.Render(0, (int)(petSize * GetDpiForWindow(pet) / 96.f)))
    return false;
  lastSpineFrame = VisualClockMs();
  ClampPetPosition();
  return true;
}
bool SetAnimation(const std::wstring &name) {
  if (name == animationName)
    return true;
  if (!spinePet.Ready() || !spinePet.SetState(name))
    return false;
  animationName = name;
  const bool drawn = DrawPet();
  RequestPetFrames(gpuProfileFrozenPet ? 0 : spinePet.DesiredFps());
  return drawn;
}
bool InitializePetRenderer(bool hardware) {
  const auto logical = animationName.empty() ? L"idle" : animationName;
  RequestPetFrames(0);
  spinePet.Shutdown();
  animationName.clear();
  auto applyStyle = [&](bool native) {
    const auto style = GetWindowLongPtrW(pet, GWL_EXSTYLE);
    SetWindowLongPtrW(pet, GWL_EXSTYLE,
        native ? (style & ~WS_EX_LAYERED) | WS_EX_NOREDIRECTIONBITMAP
               : (style & ~WS_EX_NOREDIRECTIONBITMAP) | WS_EX_LAYERED);
    SetWindowPos(pet, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  };
  spineHardware = hardware;
  applyStyle(hardware);
  if (!spinePet.Initialize(pet, hardware)) {
    if (!hardware)
      return false;
    // The optional D3D backend can recover through the same Spine software
    // renderer. Character resources are required in either path.
    spineHardware = false;
    applyStyle(false);
    if (!spinePet.Initialize(pet, false))
      return false;
  }
  if (SetAnimation(logical))
    return true;
  return hardware && InitializePetRenderer(false);
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
  for (HWND window : {pet, includePanel ? panel : nullptr}) {
    RECT bounds;
    if (!window || !IsWindowVisible(window) || !GetWindowRect(window, &bounds))
      continue;
    bounds = PopupAnimationBounds(window);
    result.push_back(bounds);
  }
  if (includeNotice)
    for (const auto &notice : notices) {
      if (!IsWindowVisible(notice->window))
        continue;
      auto bounds = PopupAnimationBounds(notice->window);
      const auto protrusion = (LONG)std::ceil(12 * notice->scale);
      bounds.left -= protrusion;
      bounds.top -= protrusion;
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
  const float scale = dpi;
  const int edge = FloatingScreenPadding(GetDpiForWindow(pet) / 96.f);
  int w = (int)(logicalWidth * scale), h = (int)(logicalHeight * scale);
  w = std::min<int>(w,
                    std::max(1L, mi.rcWork.right - mi.rcWork.left - 2 * edge));
  h = std::min<int>(h,
                    std::max(1L, mi.rcWork.bottom - mi.rcWork.top - 2 * edge));
  int x = std::clamp<int>(r.left + (r.right - r.left - w) / 2,
                          mi.rcWork.left + edge, mi.rcWork.right - w - edge);
  int y = r.top - h - gap;
  if (y < mi.rcWork.top + edge)
    y = r.bottom + gap;
  y = std::clamp<int>(y, mi.rcWork.top + edge, mi.rcWork.bottom - h - edge);
  destination = PlaceAvoidingWindows(
      {x, y}, {w, h}, mi.rcWork, FloatingObstacles(false, false), edge,
      (LONG)std::ceil(8 * GetDpiForWindow(pet) / 96.f));
  PlacePopup(window, destination, {w, h});
}
bool NoticeExitInProgress() {
  return std::any_of(notices.begin(), notices.end(), [](const auto &notice) {
    return notice->closing && IsWindowVisible(notice->window);
  });
}
void PlaceNoticeWindows(bool snap = false) {
  if (!pet)
    return;
  if (!snap && NoticeExitInProgress())
    return;
  const float scale = GetDpiForWindow(pet) / 96.f;
  const int edge = FloatingScreenPadding(scale),
            gap = (int)std::ceil(8 * scale);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(pet, MONITOR_DEFAULTTONEAREST), &monitor);
  RECT character;
  GetWindowRect(pet, &character);
  const auto panelBounds =
      IsWindowVisible(panel) ? PopupAnimationBounds(panel) : character;
  auto occupied = FloatingObstacles(true, false);
  for (const auto &notice : notices)
    if (notice->closing && IsWindowVisible(notice->window)) {
      auto bounds = PopupAnimationBounds(notice->window);
      occupied.push_back(bounds);
    }
  for (auto &notice : notices) {
    if (notice->closing)
      continue;
    if (notice->scale != scale)
      notice->target.Reset();
    notice->scale = scale;
    const float contentScale = scale * panelZoom;
    const int width = std::min(
        IsWindowVisible(panel) ? (int)(panelBounds.right - panelBounds.left)
                               : (int)(PanelWidth * contentScale),
        (int)(monitor.rcWork.right - monitor.rcWork.left) - 2 * edge);
    const int height =
        std::min((int)(NoticeHeight(*notice) * contentScale),
                 (int)(monitor.rcWork.bottom - monitor.rcWork.top) - 2 * edge);
    const POINT preferred{
        IsWindowVisible(panel)
            ? panelBounds.left
            : character.left + (character.right - character.left - width) / 2,
        std::min(character.top, panelBounds.top) - height - gap};
    const auto outset = (LONG)std::ceil(12 * scale);
    notice->destination =
        PlaceAvoidingWindows(preferred, {width, height}, monitor.rcWork,
                             occupied, edge, gap, {outset, outset});
    if (snap) {
      PlacePopup(notice->window, notice->destination, {width, height});
      SnapPopupPlacement(notice->window);
    } else
      RepositionPopupAnimated(notice->window,notice->destination,{width,height});
    // 同组气泡按最终槽位规划，让所有后续气泡同时补位；对工具栏等外部窗口
    // 仍使用 PopupAnimationBounds 的整段运动范围，避免中途穿插。
    const auto bounds=FloatingFootprint(notice->destination,{width,height});
    // 同列主体之间共享 8 DIP 间距。外伸关闭钮只用于屏幕/工具栏避让，
    // 不能把它的整块包围盒和静态动画余量重复累加到每条气泡之间。
    occupied.push_back(bounds);
    InvalidateRect(notice->window, nullptr, FALSE);
  }
}
void ReflowPopups() {
  if (!pet)
    return;
  dpi = GetDpiForWindow(pet) / 96.f * panelZoom;
  scroll = std::clamp(scroll, 0,
      std::max(0, ((int)snapshot.sessions.size() - VisibleRows()) * (int)RowHeight));
  target.Reset();
  for (auto &notice : notices)
    notice->target.Reset();
  if (PopupTargetVisible(panel)) {
    PlaceFloating(panel, PanelWidth, PanelHeight(), panelDestination, 6);
    InvalidateRect(panel, nullptr, FALSE);
  }
  UpdatePanelFeedback();
  PlaceNoticeWindows();
  RefreshToolbarPlacement();
}
void MoveAttachedPopups() {
  if (PopupTargetVisible(panel)) {
    PlaceFloating(panel, PanelWidth, PanelHeight(), panelDestination, 6);
    SnapPopupPlacement(panel);
  }
  PlaceNoticeWindows(true);
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
  PlaceNoticeWindows();
  RefreshToolbarPlacement();
}
void TogglePanel() {
  if (PopupTargetVisible(panel))
    DismissPanel();
  else
    ShowPanel();
}
void UpdateAnimation() {
  if (!gpuProfileScene.empty()) { SetAnimation(L"working"); return; }
  const auto now = GetTickCount64();
  if (celebrations.Advance(now) && animationName == L"success")
    animationName.clear();
  bool running = snapshot.online &&
                 std::any_of(snapshot.sessions.begin(), snapshot.sessions.end(),
                             [](const auto &s) { return s.running; });
  SetAnimation(
      SelectPetAnimation(running, celebrations.Active(now), now < pausedUntil,
                         hover.wasInside && !(dragging && moved),
                         std::any_of(
                             snapshot.sessions.begin(), snapshot.sessions.end(),
                             [](const auto &s) { return s.waiting; })));
}
void ShowIncomingNotices() {
  if (NoticeExitInProgress())
    return; // Queue arrivals until retiring windows have released their slots.
  std::vector<HWND> added;
  while (!incomingNotices.empty()) {
    NoticeRecord record{std::move(incomingNotices.front())};
    incomingNotices.pop_front();
    if (!record.Sync(snapshot))
      continue;
    if (std::any_of(notices.begin(), notices.end(), [&](const auto &n) {
          return n->record.Key() == record.Key();
        }))
      continue;
    auto notice = std::make_unique<NoticeSurface>();
    notice->record = std::move(record);
    const DWORD style =
        WS_EX_TOPMOST |
        (preview ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    const auto caption = NoticeCaption(notice->record);
    notice->window = CreateWindowExW(style, L"ZCodePet.Completion",
                                     caption.c_str(), WS_POPUP, 0, 0, 320, 50,
                                     pet, nullptr, instance, nullptr);
    if (!notice->window)
      continue;
    const auto window = notice->window;
    notices.push_back(std::move(notice));
    InitNoticeClose(window, preview);
    ApplySurfaceMaterial(window);
    added.push_back(window);
  }
  if (!added.empty()) {
    PlaceNoticeWindows();
    for (const auto window : added) {
      auto notice = FindNotice(window);
      ShowPopupAnimated(window, notice->destination, notice->scale);
    }
    RefreshToolbarPlacement();
  }
}
void CloseNotice(HWND window) {
  auto notice = FindNotice(window);
  if (!notice || notice->closing)
    return;
  notice->closing = true;
  notice->pressed.reset();
  notice->hoverUntil = 0;
  RetireNoticeClose(window);
  SetInteractionFeedback(window, L"body", {}, 0);
  HidePopupAnimated(window, notice->scale);
  // Do not route survivors around a temporary closing-window obstacle, then
  // route them back again. Hold current positions until all exits complete.
  for (const auto &other : notices)
    if (!other->closing)
      FreezePopupPlacement(other->window);
  RefreshToolbarPlacement();
}
void ReapNotices() {
  bool removed = false;
  for (auto i = notices.begin(); i != notices.end();) {
    auto &notice = **i;
    if (notice.closing && !IsWindowVisible(notice.window)) {
      DestroyNoticeClose(notice.window);
      ClearInteractionFeedback(notice.window);
      UiRenderer::Forget(notice.window);
      ForgetPopupAnimation(notice.window);
      DestroyWindow(notice.window);
      i = notices.erase(i);
      removed = true;
    } else
      ++i;
  }
  if (removed) {
    if (!NoticeExitInProgress())
      ShowIncomingNotices();
    PlaceNoticeWindows();
    RefreshToolbarPlacement();
  }
}
bool NoticeReflowTests() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSW type{};
  type.hInstance=GetModuleHandleW(nullptr);
  type.lpfnWndProc=DefWindowProcW;
  type.lpszClassName=L"ZCodePet.ReflowFixture";
  RegisterClassW(&type);
  RECT work{};
  if (!SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0))
    return false;
  auto make=[&](int x,int y,int w,int h) {
    auto window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED,type.lpszClassName,
        L"",WS_POPUP,x,y,w,h,nullptr,nullptr,type.hInstance,nullptr);
    SetLayeredWindowAttributes(window,0,0,LWA_ALPHA);
    return window;
  };
  const int center=(work.left+work.right)/2;
  pet=make(center-40,work.bottom-100,80,80);
  panel=make(center-200,work.bottom-250,400,120);
  bool okay=pet && panel;
  ShowWindow(pet,SW_SHOWNOACTIVATE);
  ShowWindow(panel,SW_SHOWNOACTIVATE);
  auto rect=[](HWND window){RECT r{};GetWindowRect(window,&r);return r;};
  for(bool closeUpper : {true,false}) {
    for(int i=0;i<2;++i) {
      auto notice=std::make_unique<NoticeSurface>();
      notice->window=make(0,0,1,1);
      okay &= notice->window!=nullptr;
      notices.push_back(std::move(notice));
    }
    PlaceNoticeWindows(true);
    for(const auto &notice:notices) {
      ShowPopupAnimated(notice->window,notice->destination,notice->scale);
      SnapPopupPlacement(notice->window);
    }
    const HWND upper=rect(notices[0]->window).top<rect(notices[1]->window).top
                        ? notices[0]->window : notices[1]->window;
    const HWND lower=upper==notices[0]->window ? notices[1]->window : notices[0]->window;
    const HWND closing=closeUpper ? upper : lower, survivor=closeUpper ? lower : upper;
    const auto before=rect(survivor);
    CloseNotice(closing);
    // Simulate an observer/rename layout pass while the other window exits.
    PlaceNoticeWindows();
    const auto during=rect(survivor);
    okay &= before.left==during.left && before.top==during.top;
    HidePopupImmediately(closing);
    ReapNotices();
    const auto started=rect(survivor);
    okay &= before.left==started.left && before.top==started.top;
    LONG last=started.top;
    for(int frame=0;frame<30;++frame) {
      Sleep(10);
      PulsePopupAnimations();
      const auto current=rect(survivor);
      okay &= current.left==before.left;
      okay &= closeUpper ? current.top==before.top : current.top>=last;
      last=current.top;
    }
    if(!closeUpper) okay &= last>before.top;
    ForgetPopupAnimation(survivor);
    ClearInteractionFeedback(survivor);
    DestroyWindow(survivor);
    notices.clear();
  }
  DestroyWindow(panel);DestroyWindow(pet);panel=pet=nullptr;
  ClearPopupAnimations();ClearAllInteractionFeedback();
  return okay;
}
void UpdateNoticeHovers() {
  POINT p;
  GetCursorPos(&p);
  const auto under = WindowFromPoint(p);
  const auto now = GetTickCount64();
  for (auto &notice : notices) {
    const bool visible = !notice->closing && PopupTargetVisible(notice->window);
    if (visible &&
        (under == notice->window || IsNoticeClose(notice->window, under)))
      notice->hoverUntil = now + 160;
    ShowNoticeClose(notice->window, notice->scale,
                    visible && now < notice->hoverUntil);
    UpdateNoticeFeedback(notice->window);
  }
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
  if (!gpuProfileScene.empty()) show=!gpuProfilePetOnly;
  if (show && !PopupTargetVisible(panel))
    ShowPanel();
  if (!show && PopupTargetVisible(panel))
    HidePopupAnimated(panel, GetDpiForWindow(pet) / 96.f);
  ReapNotices();
  ShowIncomingNotices();
  UpdateNoticeHovers();
  UpdatePanelFeedback();
  UpdateAnimation();
}
bool TickPanel(bool visualFrame = false) {
  static std::vector<std::wstring> previousAges;
  static PanelFramePolicy frames;
  if (!IsWindowVisible(panel)) {
    panelComposition.Suspend();
    lastPanelScene.reset();
    frames.hadMotion = false;
    RequestContinuousFrames(false);
    return false;
  }
  // Repeating animations belong to the system compositor. The UI frame clock
  // is needed only for a finite hop or the compatibility renderer.
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
  // 动画由垂直同步独占重绘，避免 100 ms 定时器叠加成 120 + 10 Hz。
  // Keep one final resting frame pending until the visual clock consumes it.
  const bool moving = (loading && !panelComposition.Ready()) || transitioning;
  if (frames.Tick(moving, ages != previousAges, visualFrame)) {
    InvalidateRect(panel, nullptr, FALSE);
    previousAges = std::move(ages);
  }
  const bool continuous = frames.NeedsFrames(moving);
  RequestContinuousFrames(continuous);
  return continuous;
}
void PaintCapsule(HWND window) {
  ++noticeDraws;
  auto notice = FindNotice(window);
  if (!notice)
    return;
  const bool capsuleAttention = notice->record.Attention();
  const auto capsuleTitle = notice->record.Title(),
             capsuleRecap = notice->record.Detail();
  const auto savedDpi = dpi;
  dpi = notice->scale * panelZoom;
  auto saved = target;
  target = notice->target;
  PAINTSTRUCT ps;
  BeginPaint(window, &ps);
  RECT rc;
  GetClientRect(window, &rc);
  if (!target) {
    UiRenderer::Create(factory.Get(), window, 96 * dpi,
                       D2D1::SizeU(rc.right, rc.bottom), &target);
  }
  if (target) {
    target->BeginDraw();
    const float width = rc.right / dpi, height = rc.bottom / dpi;
    PetTheme::PaintSurface(target.Get(), width, height,
                           SUCCEEDED(notice->material), 8.f, false);
    PetTheme::PaintFeedback(target.Get(), D2D1::RectF(0, 0, width, height),
                            InteractionOpacity(window, L"body"), 0);
    const float badgeY = height / 2;
    const float badgeX = RowIndicatorCenter, glyphScale = 1.f / panelZoom;
    ComPtr<ID2D1SolidColorBrush> green;
    const auto tone = StateTone(capsuleAttention ? SessionState::Waiting
                                                 : SessionState::Completed);
    target->CreateSolidColorBrush(D2D1::ColorF(tone.fill), &green);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(badgeX, badgeY),
                                      10 * glyphScale, 10 * glyphScale),
                        green.Get());
    green->SetColor(D2D1::ColorF(tone.ink));
    if (capsuleAttention) {
      target->DrawLine(D2D1::Point2F(badgeX, badgeY - 4 * glyphScale),
                       D2D1::Point2F(badgeX, badgeY + glyphScale), green.Get(),
                       2.f * glyphScale,RoundStatusStroke(factory.Get()));
      target->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(badgeX, badgeY + 4.5f * glyphScale),
                        1.1f*glyphScale, 1.1f*glyphScale),
          green.Get());
    } else {
      DrawCheckMark(target.Get(),factory.Get(),green.Get(),badgeX,badgeY,
                      2.5f*glyphScale,2.f*glyphScale);
    }
    std::wstring title = capsuleTitle.empty() ? L"未命名会话" : capsuleTitle;
    if (title.size() > 44) {
      title.resize(44);
      if (title.back() >= 0xd800 && title.back() <= 0xdbff)
        title.pop_back();
      title += L"…";
    }
    const auto content =
        title + (capsuleRecap.empty()
                     ? capsuleAttention ? L" · 请求回答" : L" · 已完成"
                     : L" · " + capsuleRecap);
    ComPtr<IDWriteTextFormat> format;
    writeFactory->CreateTextFormat(
        L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, NoticeFontSize(),
        L"zh-CN", &format);
    if (format) {
      format->SetWordWrapping(capsuleRecap.empty()
                                  ? DWRITE_WORD_WRAPPING_NO_WRAP
                                  : DWRITE_WORD_WRAPPING_WRAP);
      format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                             NoticeLineHeight(), 13.5f);
      DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      ComPtr<IDWriteInlineObject> sign;
      writeFactory->CreateEllipsisTrimmingSign(format.Get(), &sign);
      format->SetTrimming(&trim, sign.Get());
      const float textHeight =
          NoticeLineHeight() * (capsuleRecap.empty() ? 1 : 2);
      ComPtr<IDWriteTextLayout> layout;
      writeFactory->CreateTextLayout(
          content.c_str(), (UINT32)content.size(), format.Get(),
          width - RowTitleLeft - RowSidePadding, textHeight, &layout);
      if (layout) {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_TEXT_RANGE{0, (UINT32)title.size()});
        ComPtr<ID2D1SolidColorBrush> body, heading;
        target->CreateSolidColorBrush(D2D1::ColorF(PetTheme::Muted), &body);
        target->CreateSolidColorBrush(D2D1::ColorF(PetTheme::Ink), &heading);
        layout->SetDrawingEffect(heading.Get(),
                                 DWRITE_TEXT_RANGE{0, (UINT32)title.size()});
        const float y =
            CenteredTextOriginY(factory.Get(), layout.Get(), height);
        const float clipY = (height - textHeight) / 2;
        target->PushAxisAlignedClip(D2D1::RectF(RowTitleLeft, clipY,
                                                width - RowSidePadding,
                                                clipY + textHeight),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (experimental) {
          auto renderer = new CoverageRenderer(
              target.Get(), D2D1::ColorF(PetTheme::Muted), dpi);
          layout->Draw(nullptr, renderer, RowTitleLeft, y);
          renderer->Release();
        } else {
          target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
          target->DrawTextLayout(TextRasterOrigin(target.Get(), RowTitleLeft, y), layout.Get(),
                                 body.Get(), StableTextOptions);
        }
        target->PopAxisAlignedClip();
      }
    }
    PetTheme::PaintSurfaceBorder(target.Get(), width, height);
    if (target->EndDraw() == D2DERR_RECREATE_TARGET)
      target.Reset();
  }
  EndPaint(window, &ps);
  notice->target = target;
  target = saved;
  dpi = savedDpi;
}
LRESULT CALLBACK CapsuleProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  auto notice = FindNotice(h);
  if (!notice)
    return DefWindowProcW(h, msg, w, l);
  switch (msg) {
  case PopupBackendQuery:
    if (w == 1 && notice->target) {
      float x, y;
      notice->target->GetDpi(&x, &y);
      return (LRESULT)std::lround(x * 100);
    }
    return notice->target ? UiRenderer::Backend(h) : -1;
  case WM_SIZE:
    if (notice->target)
      notice->target->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  case WM_NCACTIVATE:
    // Win32 默认非激活处理会把背景亚克力退成实色；保持视觉活跃，不改变焦点。
    return DefWindowProcW(h, WM_NCACTIVATE, TRUE, l);
  case WM_THEMECHANGED:
  case WM_DWMCOMPOSITIONCHANGED:
  case WM_SETTINGCHANGE:
  case BackdropChanged:
    ApplySurfaceMaterial(h);
    panelTexture.Reset();
    InvalidateRect(h, nullptr, FALSE);
    return 0;

  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    PaintCapsule(h);
    return 0;
  case WM_SETCURSOR:
    if (LOWORD(l) == HTCLIENT) {
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    }
    break;
  case WM_LBUTTONDOWN:
    if (notice->closing)
      return 0;
    notice->pressed = notice->record.event;
    SetCapture(h);
    UpdateNoticeFeedback(h);
    return 0;
  case WM_LBUTTONUP: {
    auto pressed = notice->pressed;
    notice->pressed.reset();
    if (GetCapture() == h)
      ReleaseCapture();
    UpdateNoticeFeedback(h);
    RECT bounds;
    GetClientRect(h, &bounds);
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if (pressed && SameSession(*pressed, notice->record.event) &&
        PtInRect(&bounds, point) && !notice->closing && PopupTargetVisible(h))
      OpenSession(*pressed);
    return 0;
  }
  case WM_CAPTURECHANGED:
    notice->pressed.reset();
    UpdateNoticeFeedback(h);
    return 0;
  case WM_MOUSEMOVE: {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
    TrackMouseEvent(&track);
    UpdateNoticeFeedback(h);
    return 0;
  }
  case WM_MOUSELEAVE:
    SetInteractionFeedback(h, L"body", {}, 0);
    return 0;
  case WM_CLOSE:
    CloseNotice(h);
    return 0;
  case WM_DPICHANGED:
    notice->scale = HIWORD(w) / 96.f;
    notice->target.Reset();
    return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}
ToolbarState ToolbarValues() {
  return {panelPinned, experimental, (int)petSize,
          (int)std::lround(panelZoom * 100), strength, backdropMode,sessionRows};
}
void ExecuteToolbarAction(int id) {
  if(id==60 || id==61)ToggleDataSource(id==60?SourceId::ZCode:SourceId::DeepSeek);
  if (id >= 50 && id <= 52) {
    sessionRows = id == 52 ? DefaultSessionRows
                          : ClampSessionRows(sessionRows + (id == 51 ? 1 : -1));
    scroll = 0;
    ReflowPopups();
    SavePreferences();
  }
  if (id >= 30 && id <= 32) {
    backdropMode = ReadBackdropMode(id-30);
    SendMessageW(panel,BackdropChanged,0,0);
    for (const auto &notice : notices)
      SendMessageW(notice->window,BackdropChanged,0,0);
    RefreshToolbarMaterial();
    SavePreferences();
  }
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
  if (id == 3) {
    ShowEmbeddedLicenses(pet);
  }
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
  case WM_SHOWWINDOW:
    if (!w) { panelComposition.Suspend();lastPanelScene.reset(); }
    break;
  case PopupBackendQuery:
    if (panelComposition.Ready())
      return w==1 ? (LRESULT)std::lround(96*dpi*100) : D2D1_RENDER_TARGET_TYPE_HARDWARE;
    if (w==1 && target) { float x,y; target->GetDpi(&x,&y); return (LRESULT)std::lround(x*100); }
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
  case BackdropChanged:
    ApplySurfaceMaterial(h);
    panelTexture.Reset();
    lastPanelScene.reset();
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
    UpdatePanelFeedback();
    return 0;
  case WM_LBUTTONUP: {
    auto pressed = panelPressed;
    panelPressed.reset();
    const auto released = HitSession(h, l);
    if (GetCapture() == h)
      ReleaseCapture();
    UpdatePanelFeedback();
    if (pressed && released && SameSession(*pressed, *released))
      OpenSession(*pressed);
    return 0;
  }
  case WM_CAPTURECHANGED:
    panelPressed.reset();
    UpdatePanelFeedback();
    return 0;
  case WM_MOUSEMOVE: {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
    TrackMouseEvent(&track);
    UpdatePanelFeedback();
    return 0;
  }
  case WM_MOUSELEAVE:
    if (!panelHotKey.empty()) SetInteractionFeedback(h,panelHotKey,{},0);
    panelHotKey.clear();
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
    UpdatePanelFeedback();
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  }
  }
  return DefWindowProcW(h, msg, w, l);
}
LRESULT CALLBACK PetProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (taskbarCreatedMessage && msg==taskbarCreatedMessage) {
    trayRegistered=false;UpdateTrayIcon();return 0;
  }
  switch (msg) {
  case SessionRowsQuery:
    return w == 0 ? sessionRows : VisibleRows();
  case SpineInfoQuery:
    return spinePet.Metric((unsigned)w);
  case TrayInfoQuery:
    return w==0 ? trayRegistered : w==1 ? trayIcon!=nullptr : trayVersion4;
  case CompositionQuery:
    return panelComposition.Metric((unsigned)w);
  case GpuRenderQuery:
    if (w == 4)
      return (LRESULT)std::llround(VisualClockMs() * 1000);
    if (w == 5) return (LRESULT)panelCacheBuilds;
    if (w == 6) return (LRESULT)panelCacheHits;
    if (w == 7) return (LRESULT)panelCacheFallbacks;
    if (w == 8) return (LRESULT)backdropMode;
    if (w == 9) {
      DWM_SYSTEMBACKDROP_TYPE actual=DWMSBT_AUTO;
      return SUCCEEDED(DwmGetWindowAttribute(panel,DWMWA_SYSTEMBACKDROP_TYPE,
          &actual,sizeof(actual))) ? actual : -1;
    }
    if (w == 10) return panelDrawResult;
    if (w == 11) return (LRESULT)panelDrawErrors;
    return w==0 ? (LRESULT)panelDraws : w==1 ? (LRESULT)noticeDraws
         : w==2 ? (LRESULT)petDraws : (LRESULT)visualFrames;
  case NoticeInfoQuery: {
    if (w == 5)
      return (LRESULT)celebrations.pending;
    if (w == 6)
      return animationName == L"success"   ? 2
             : animationName == L"working" ? 1
             : animationName == L"waiting" ? 3
             : animationName == L"paused"  ? 4
                                           : 0;
    size_t totalNotices = 0;
    for (const auto &notice : notices) {
      if (w == 3)
        totalNotices += notice->closing;
      else if (!notice->closing && IsWindowVisible(notice->window))
        totalNotices += w == 0 || (w == 1 && !notice->record.Attention()) ||
                        (w == 2 && notice->record.Attention());
    }
    return (LRESULT)totalNotices;
  }
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
    ++visualFrames;
    BeginVisualFrame();
    if(spinePet.Ready() && !gpuProfileFrozenPet) {
      const auto now=VisualClockMs();
      const auto interval=1000./spinePet.DesiredFps();
      if(w==1 || now-lastSpineFrame>=interval-.25) {
        const auto delta=(float)((now-lastSpineFrame)/1000.);
        if(spinePet.Render(delta,(int)(petSize*GetDpiForWindow(pet)/96.f))) {
          ++petDraws;lastSpineFrame=now;
        } else if (!InitializePetRenderer(false)) {
          MessageBoxW(pet, L"Spine 角色渲染失败，请重新启动桌宠。",
                      L"ZCode Pet", MB_ICONERROR);
          DestroyWindow(pet);
          return 0;
        }
      }
      RequestPetFrames(spinePet.Ready() ? spinePet.DesiredFps() : 0);
    }
    PulsePopupAnimations();
    TickPanel(true);
    PulseInteractionFeedback();
    if (IsWindowVisible(panel))
      UpdateWindow(panel);
    UpdateNoticeHovers();
    return 0;
  case VisualFrameInfoQuery:
    if(w==5)return FrameClockPetFps();
    if(w==6)return FrameClockContinuousRequested();
    if (w == 3)
      return FrameClockInteractionRequested();
    if (w == 4)
      return FrameClockFramesNeeded();
    return w == 0 ? FrameClockDisplayHz()
         : w == 1 ? FrameClockUsesVBlank()
                  : (LRESULT)std::lround(FrameClockAverageMs() * 100);
  case PopupFrameTimeQuery:
    if (w!=0) return PopupReflowMetric(w);
    return (LRESULT)std::lround(PopupAverageFrameMs() * 100);
  case WM_TIMER:
    if (w == 2) {
      HoverTick();
      return 0;
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
      if (!moved && abs(dx) + abs(dy) > 5) {
        moved = true;
        UpdateAnimation();
        if (spinePet.PlayDragBounce()) {
          DrawPet();
          RequestPetFrames(spinePet.DesiredFps());
        }
      }
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
      UpdateAnimation();
      DrawPet();
      MoveAttachedPopups();
      if (!moved)
        ShowPanel();
    }
    return 0;
  case WM_CAPTURECHANGED:
    if (dragging) {
      dragging = false;
      UpdateAnimation();
      DrawPet();
      MoveAttachedPopups();
    }
    return 0;
  case WM_DPICHANGED:
    UpdateTrayIcon(true);
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
    if ((trayVersion4 && (LOWORD(l)==NIN_SELECT || LOWORD(l)==NIN_KEYSELECT)) ||
        (!trayVersion4 && l==WM_LBUTTONUP))
      TogglePanel();
    if ((trayVersion4 && LOWORD(l)==WM_CONTEXTMENU) || (!trayVersion4 && l==WM_RBUTTONUP))
      Menu();
    return 0;
  case Refresh: {
    static CompletionTracker completions(appStartedAt);
    static AttentionTracker attention;
    {
      std::lock_guard lock(dataMutex);
      for (auto &session : attention.Accept(pending)) {
        incomingNotices.push_back(session);
      }
      auto finished = completions.Accept(pending);
      celebrations.Push(finished.size());
      for (auto &session : finished)
        incomingNotices.push_back(session);
      for (auto &notice : notices)
        if (!notice->closing) {
          const auto previousTitle = notice->record.Title(),previousDetail=notice->record.Detail();
          if (!notice->record.Sync(pending))
            CloseNotice(notice->window);
          else {
            if (previousTitle != notice->record.Title())
              SetWindowTextW(notice->window,
                             NoticeCaption(notice->record).c_str());
            if (previousTitle!=notice->record.Title() || previousDetail!=notice->record.Detail())
              InvalidateRect(notice->window, nullptr, FALSE);
          }
        }
      if (snapshot.online && pending.online)
        for (const auto &before : snapshot.sessions)
          if (before.running)
            for (const auto &after : pending.sessions)
              if (after.id == before.id &&
                  after.workspace == before.workspace && !after.running &&
                  after.endedAt > 0 &&
                  (after.result == L"error" || after.result == L"interrupted"))
                pausedUntil = GetTickCount64() + 4000;
      stateMotion.Update(pending, (uint64_t)VisualClockMs(),
                         IsWindowVisible(panel));
      StabilizeActiveSessionOrder(pending, snapshot);
      snapshot = std::move(pending);
      stateClock.Update(snapshot, WallClockMs());
      if (navigationPreview && !snapshot.sessions.empty()) {
        // 显式本地验收入口，只生成一条已有会话的预览提醒，不伪造 ZCode 状态。
        const auto ready = std::find_if(
            snapshot.sessions.begin(), snapshot.sessions.end(),
            [](const auto &s) {
              return !s.running && !s.waiting && s.result == L"completed";
            });
        if (ready != snapshot.sessions.end())
          incomingNotices.push_back(*ready);
        navigationPreview = false;
      }
      scroll = std::clamp(
          scroll, 0,
          std::max(0, ((int)snapshot.sessions.size() - VisibleRows()) *
                          (int)RowHeight));
      UpdatePanelFeedback();
    }
    if (IsWindowVisible(panel)) {
      RECT r;
      GetWindowRect(panel, &r);
      int height = (int)(PanelHeight() * dpi);
      if (r.bottom - r.top != height) {
        ReflowPopups();
      }
    }
    ShowIncomingNotices();
    UpdateAnimation();
    if (!TickPanel() && IsWindowVisible(panel)) {
      RECT bounds{};
      GetClientRect(panel,&bounds);
      const auto scene=CapturePanelScene(bounds.right/dpi,bounds.bottom/dpi);
      if (!lastPanelScene || *lastPanelScene!=scene)
        InvalidateRect(panel,nullptr,FALSE);
    }
    return 0;
  }
  case WM_DESTROY:
    quitting = true;
    {
      NOTIFYICONDATAW n{sizeof(n)};
      n.hWnd = h;
      n.uID = 1;
      Shell_NotifyIconW(NIM_DELETE, &n);
      trayRegistered=false;
      if (trayIconOwned && trayIcon) DestroyIcon(trayIcon);
      trayIcon=nullptr;trayIconOwned=false;
    }
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR command, int) {
  InitializeDataSources();
  if(wcsstr(command,L"--harness-navigation-smoke"))return HarnessNavigationSmoke()?0:1;
  if(wcsstr(command,L"--source-diagnostic")){std::ofstream f("source-diagnostic.csv");f<<"sample,harness,running,completed,unknown,warning\n";for(int i=0;i<15;++i){auto snap=Observe();int count=0,running=0,completed=0,unknown=0;for(auto& row:snap.sessions)if(row.source==SourceId::DeepSeek){++count;running+=row.running;completed+=row.result==L"completed";unknown+=row.uncertain;}f<<i<<","<<count<<","<<running<<","<<completed<<","<<unknown<<","<<!snap.sourceWarning.empty()<<"\n";f.flush();Sleep(1000);}return 0;}
  if(wcsstr(command,L"--source-benchmark"))return DataSourceTests(true)?0:1;
  if(wcsstr(command,L"--data-sources-test"))return DataSourceTests()?0:1;
  if(wcsstr(command,L"--notice-reflow-test"))return NoticeReflowTests() ? 0 : 1;
  if(wcsstr(command,L"--frame-clock-test"))return FrameClockTimingTests() ? 0 : 1;
  if(wcsstr(command,L"--raster-test"))return SoftwareRasterTests() ? 0 : 1;
  if (wcsstr(command,L"--composition-test")) return PanelCompositionTests() ? 0 : 1;
  if (wcsstr(command,L"--panel-scene-test")) return PanelSceneTests() ? 0 : 1;
  instance = inst;
  wchar_t path[32768];
  GetModuleFileNameW(nullptr, path, 32768);
  base = fs::path(path).parent_path();
  if(wcsstr(command,L"--observer-benchmark")) {
    std::ofstream results(base/L"observer-benchmark.csv");
    if(!results)return 2;
    results << "sample,cpuUs,processUs,indexUs,metadataUs,logUs,fallbackUs,questionsUs,totalUs,error,online\n";
    auto cpu=[] {
      FILETIME created,ended,kernel,user;
      GetThreadTimes(GetCurrentThread(),&created,&ended,&kernel,&user);
      ULARGE_INTEGER k{},u{};
      k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
      u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;
      return (k.QuadPart+u.QuadPart)/10;
    };
    for(int sample=0;sample<24;++sample) {
      const auto before=cpu();
      const auto observed=ObserveZCode();
      const auto used=cpu()-before;
      const auto t=LastObserverTiming();
      results << sample << ',' << used << ',' << t.processUs << ',' << t.indexUs
        << ',' << t.metadataUs << ',' << t.logUs << ',' << t.fallbackUs << ','
        << t.questionsUs << ',' << t.totalUs << ',' << !observed.error.empty()
        << ',' << observed.online << '\n';
      Sleep(50);
    }
    return 0;
  }
  if(wcsstr(command,L"--licenses")){ ShowEmbeddedLicenses(nullptr); return 0; }
  if(wcsstr(command,L"--embedded-assets-test"))return EmbeddedAssetsTests() ? 0 : 1;
  if(wcsstr(command,L"--spine-lifecycle-test"))return SpineLifecycleTests() ? 0 : 1;
  if(wcsstr(command,L"--spine-model-test"))return SpineModelTests() ? 0 : 1;
  if (wcsstr(command,L"--spine-render-test"))
    return RenderSpinePreview(base/(wcsstr(command,L"--software") ? L"spine-preview-soft" : L"spine-preview"),
                               !wcsstr(command,L"--software")) ? 0 : 1;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (wcsstr(command,L"--notice-test")) return NoticeModelTests() ? 0 : 1;
  if (wcsstr(command, L"--interaction-motion-test"))
    return InteractionMotionTests() ? 0 : 1;
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
    f << "projectMetadataKnown="
      << std::count_if(s.sessions.begin(), s.sessions.end(),
                      [](const auto &row) { return row.projectMetadataKnown; })
      << " projectDisplayNames="
      << std::count_if(s.sessions.begin(), s.sessions.end(),
                      [](const auto &row) { return !row.projectDisplayName.empty(); })
      << " projectDisplayOverrides="
      << std::count_if(s.sessions.begin(), s.sessions.end(), [](const auto &row) {
           return !row.projectDisplayName.empty() &&
                  !SameProject(row.projectDisplayName, ProjectName(row.path));
         }) << "\n";
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
    std::vector<HWND> noticeWindows;
    for (HWND window =
             FindWindowExW(nullptr, nullptr, L"ZCodePet.Completion", nullptr);
         window; window = FindWindowExW(nullptr, window, L"ZCodePet.Completion",
                                        nullptr))
      if (IsWindowVisible(window))
        noticeWindows.push_back(window);
    HWND notice = noticeWindows.empty() ? nullptr : noticeWindows.front();
    RECT panelGeometry{};
    GetWindowRect(FindWindowW(L"ZCodePet.Sessions", L"ZCode 会话"),
                  &panelGeometry);
    LONG minNoticeWidth = MAXLONG, maxNoticeWidth = 0;
    for (const auto window : noticeWindows) {
      RECT bounds{};
      GetWindowRect(window, &bounds);
      minNoticeWidth = std::min(minNoticeWidth, bounds.right - bounds.left);
      maxNoticeWidth = std::max(maxNoticeWidth, bounds.right - bounds.left);
    }
    f << "panelWidth=" << panelGeometry.right - panelGeometry.left
      << " panelHeight=" << panelGeometry.bottom - panelGeometry.top
      << " noticeMinWidth=" << (noticeWindows.empty() ? 0 : minNoticeWidth)
      << " noticeMaxWidth=" << maxNoticeWidth << "\n";
    std::vector<RECT> column;
    if (IsWindowVisible(FindWindowW(L"ZCodePet.Sessions",L"ZCode 会话"))) column.push_back(panelGeometry);
    for (const auto window:noticeWindows) { RECT bounds{}; GetWindowRect(window,&bounds); column.push_back(bounds); }
    std::sort(column.begin(),column.end(),[](const auto &a,const auto &b){return a.top<b.top;});
    LONG minimumGap=MAXLONG,maximumGap=0; size_t gapPairs=0;
    for (size_t i=1;i<column.size();++i) {
      const auto &a=column[i-1],&b=column[i];
      if (a.left==b.left && a.right==b.right && b.top>=a.bottom) {
        const auto value=b.top-a.bottom;
        minimumGap=std::min(minimumGap,value); maximumGap=std::max(maximumGap,value); ++gapPairs;
      }
    }
    f << "noticeGapMin=" << (gapPairs ? minimumGap : 0)
      << " noticeGapMax=" << maximumGap << " noticeGapPairs=" << gapPairs << "\n";
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
                        FindWindowW(L"ZCodePet.Toolbar", L"桌宠工具栏")}) {
      RECT bounds;
      if (window && IsWindowVisible(window) && GetWindowRect(window, &bounds))
        visibleWindows.push_back(bounds);
    }
    for (const auto window:noticeWindows) {
      RECT bounds; if (GetWindowRect(window,&bounds)) visibleWindows.push_back(bounds);
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
    f << "panelRenderDpi=" << SendMessageW(FindWindowW(L"ZCodePet.Sessions",L"ZCode 会话"),PopupBackendQuery,1,0)/100.
      << " noticeRenderDpi=" << SendMessageW(notice,PopupBackendQuery,1,0)/100. << "\n";
    f << "displayHz=" << SendMessageW(character, VisualFrameInfoQuery, 0, 0)
      << " vblankClock=" << SendMessageW(character, VisualFrameInfoQuery, 1, 0)
      << " visualFrameAverageMs="
      << SendMessageW(character, VisualFrameInfoQuery, 2, 0) / 100. << "\n";
    f << "interactionFrames="
      << SendMessageW(character, VisualFrameInfoQuery, 3, 0)
      << " visualFramesNeeded="
      << SendMessageW(character, VisualFrameInfoQuery, 4, 0) << "\n";
    f << "reflowStarts=" << SendMessageW(character,PopupFrameTimeQuery,4,0)
      << " reflowFrames=" << SendMessageW(character,PopupFrameTimeQuery,1,0)
      << " reflowAverageMs=" << SendMessageW(character,PopupFrameTimeQuery,2,0)/100.
      << " reflowActive=" << SendMessageW(character,PopupFrameTimeQuery,3,0) << "\n";
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
    f << "noticesVisible=" << SendMessageW(character, NoticeInfoQuery, 0, 0)
      << " completedNotices=" << SendMessageW(character, NoticeInfoQuery, 1, 0)
      << " waitingNotices=" << SendMessageW(character, NoticeInfoQuery, 2, 0)
      << " closingNotices=" << SendMessageW(character, NoticeInfoQuery, 3, 0)
      << " celebrationsPending="
      << SendMessageW(character, NoticeInfoQuery, 5, 0)
      << " petAnimation=" << SendMessageW(character, NoticeInfoQuery, 6, 0)
      << "\n";
    f << "panelDraws=" << SendMessageW(character,GpuRenderQuery,0,0)
      << " noticeDraws=" << SendMessageW(character,GpuRenderQuery,1,0)
      << " petDraws=" << SendMessageW(character,GpuRenderQuery,2,0)
      << " visualFrames=" << SendMessageW(character,GpuRenderQuery,3,0)
      << " renderTimeMicros=" << SendMessageW(character,GpuRenderQuery,4,0) << "\n";
    f << "panelCacheBuilds=" << SendMessageW(character,GpuRenderQuery,5,0)
      << " panelCacheHits=" << SendMessageW(character,GpuRenderQuery,6,0)
      << " panelCacheFallbacks=" << SendMessageW(character,GpuRenderQuery,7,0)
      << " backdropMode=" << SendMessageW(character,GpuRenderQuery,8,0)
      << " backdropType=" << SendMessageW(character,GpuRenderQuery,9,0) << "\n";
    f << "panelDrawResult=" << SendMessageW(character,GpuRenderQuery,10,0)
      << " panelDrawErrors=" << SendMessageW(character,GpuRenderQuery,11,0) << "\n";
    f << "compositionReady=" << SendMessageW(character,CompositionQuery,0,0)
      << " compositionResult=" << SendMessageW(character,CompositionQuery,1,0)
      << " compositionUploads=" << SendMessageW(character,CompositionQuery,2,0)
      << " compositionStarts=" << SendMessageW(character,CompositionQuery,3,0)
      << " compositionUpdates=" << SendMessageW(character,CompositionQuery,4,0)
      << " compositionRows=" << SendMessageW(character,CompositionQuery,5,0)
      << " compositionFailures=" << SendMessageW(character,CompositionQuery,6,0) << "\n";
    f << "compositionBaseDraws=" << SendMessageW(character,CompositionQuery,7,0) << "\n";
    f << "trayRegistered=" << SendMessageW(character,TrayInfoQuery,0,0)
      << " trayIconLoaded=" << SendMessageW(character,TrayInfoQuery,1,0)
      << " trayVersion4=" << SendMessageW(character,TrayInfoQuery,2,0) << "\n";
    f << "spineReady=" << SendMessageW(character,SpineInfoQuery,0,0)
      << " spineResult=" << SendMessageW(character,SpineInfoQuery,1,0)
      << " spineFrames=" << SendMessageW(character,SpineInfoQuery,2,0)
      << " spineVertices=" << SendMessageW(character,SpineInfoQuery,3,0)
      << " spineBatches=" << SendMessageW(character,SpineInfoQuery,4,0)
      << " spineTransitions=" << SendMessageW(character,SpineInfoQuery,5,0)
      << " spineErrors=" << SendMessageW(character,SpineInfoQuery,6,0)
      << " spineHardware=" << SendMessageW(character,SpineInfoQuery,7,0) << "\n";
    f << "petRequestedFps=" << SendMessageW(character,VisualFrameInfoQuery,5,0)
      << " panelContinuousFrames=" << SendMessageW(character,VisualFrameInfoQuery,6,0) << "\n";
    f << "sessionRowLimit=" << SendMessageW(character,SessionRowsQuery,0,0)
      << " sessionVisibleRows=" << SendMessageW(character,SessionRowsQuery,1,0) << "\n";
    return s.error.empty() ? 0 : 2;
  }
  if (wcsstr(command, L"--quit")) {
    HWND existing = FindWindowW(L"ZCodePet.Native", L"ZCode Pet");
    if (existing)
      PostMessageW(existing, WM_CLOSE, 0, 0);
    return 0;
  }
  demo = wcsstr(command, L"--demo") != nullptr;
  if (const auto flag=wcsstr(command,L"--gpu-profile=")) {
    const auto value=flag+wcslen(L"--gpu-profile=");
    const auto end=wcspbrk(value,L" \t");
    gpuProfileScene.assign(value,end ? end-value : wcslen(value));
    gpuProfilePetOnly=gpuProfileScene.rfind(L"pet",0)==0;
    gpuProfileFrozenPet=gpuProfileScene.find(L"frozen")!=std::wstring::npos;
    gpuProfileNoAcrylic=gpuProfileScene.find(L"no-acrylic")!=std::wstring::npos;
    gpuProfileNoShimmer=gpuProfileScene.find(L"no-shimmer")!=std::wstring::npos;
    gpuProfileNoSpinner=gpuProfileScene.find(L"no-spinner")!=std::wstring::npos;
    gpuProfileNoCache=gpuProfileScene.find(L"no-cache")!=std::wstring::npos;
    gpuProfileNoMask=gpuProfileScene.find(L"no-mask")!=std::wstring::npos;
    gpuProfileLegacy=gpuProfileScene.find(L"legacy")!=std::wstring::npos;
  }
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
  if (!gpuProfileScene.empty())
    backdropMode = gpuProfileScene.find(L"mica")!=std::wstring::npos ? BackdropMode::Mica
        : gpuProfileScene.find(L"solid")!=std::wstring::npos ? BackdropMode::Solid
        : BackdropMode::Acrylic;
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
  wc.hIcon=LoadIconW(inst,MAKEINTRESOURCEW(IDI_ZCODEPET));
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
  ApplySurfaceMaterial(panel);
  InitToolbar(pet, factory.Get(), writeFactory.Get(), ToolbarValues,
              ExecuteToolbarAction, ApplySurfaceMaterial, ToolbarObstacles, preview);
  UpdateTrayIcon();
  spineHardware=wcsstr(command,L"--spine-gpu") || gpuProfileScene.find(L"spine-gpu")!=std::wstring::npos;
  if (!InitializePetRenderer(spineHardware)) {
    MessageBoxW(nullptr, L"无法加载内嵌 Spine 角色资源，请重新下载完整的 EXE。",
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
    while (!quitting) {
      Snapshot s;
      try {
        if (!gpuProfileScene.empty()) {
          s.online=true;
          for (int i=0;i<3;++i) {
            Session fixture;
            fixture.id=L"gpu-profile-"+std::to_wstring(i); fixture.workspace=L"gpu-profile";
            fixture.title=L"GPU 固定场景 "+std::to_wstring(i+1); fixture.path=L"GPU profile";
            fixture.running=gpuProfileScene.rfind(L"running",0)==0;
            if (fixture.running) fixture.startedAt=appStartedAt;
            else { fixture.result=L"completed"; fixture.endedAt=appStartedAt-3600000; }
            s.sessions.push_back(std::move(fixture));
          }
        } else if (demo) {
          s.online = true;
          const auto demoAge = GetTickCount64() - demoStart;
          Session a;
          a.id = L"demo";
          a.workspace = L"demo";
          a.title =
              demoAge < 14000 ? L"桌宠状态提示更新" : L"已重命名的外观会话";
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
          if (demoAge >= 18000) {
            a.running = true;
            a.turnId = L"demo-two";
            a.startedAt = appStartedAt + 18000;
            a.endedAt = 0;
            a.result.clear();
          }
          Session second;
          second.id = L"demo-second";
          second.workspace = L"demo";
          second.title = L"第二个会话完成";
          second.turnId = L"demo-second-one";
          second.startedAt = appStartedAt;
          second.running = demoAge < 10000;
          if (!second.running) {
            second.endedAt = appStartedAt + 10000;
            second.result = L"completed";
          }
          Session active;
          active.id = L"demo-active";
          active.workspace = L"demo";
          active.title = L"持续工作的会话";
          active.turnId = L"demo-active-one";
          active.startedAt = appStartedAt;
          active.running = true;
          s.sessions = {a, second, active};
          // Fixed local fixtures make every semantic palette inspectable by
          // scrolling, without manufacturing events in the observed app.
          Session waiting;
          waiting.id = L"demo-waiting";
          waiting.turnId = L"demo-question";
          waiting.title = L"chat 等待你的回答";
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
        }
      } catch (...) {
        s.error = L"状态读取失败";
      }
      {
        std::lock_guard lock(dataMutex);
        pending = std::move(s);
      }
      PostMessageW(pet, Refresh, 0, 0);
      for (int i = 0; i < (int)(SourcePollIntervalMs()/100) && !quitting; ++i)
        Sleep(100);
    }
  });
  if (!gpuProfileScene.empty() ? !gpuProfilePetOnly : panelPinned || wcsstr(command,L"--panel"))
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
  spinePet.Shutdown();
  panelComposition.Shutdown();
  DestroyToolbar();
  ClearPopupAnimations();
  ClearAllInteractionFeedback();
  for (auto &notice:notices) {
    DestroyNoticeClose(notice->window);
    UiRenderer::Forget(notice->window);
    DestroyWindow(notice->window);
  }
  notices.clear();
  DestroyWindow(panel);
  panelTexture.Reset();
  target.Reset();
  writeFactory.Reset();
  factory.Reset();
  panelComposition.FinishThread();
  Gdiplus::GdiplusShutdown(gdip);
  CoUninitialize();
  CloseHandle(singleton);
  return 0;
}
